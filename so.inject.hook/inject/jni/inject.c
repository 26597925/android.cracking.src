#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <android/log.h>


#define ENABLE_DEBUG		1

#define CPSR_T_MASK        ( 1u << 5 )


#if defined(__aarch64__)
#define pt_regs user_pt_regs
#define uregs regs  
#define ARM_pc  pc  
#define ARM_sp  sp  
#define ARM_cpsr    pstate  
#define ARM_lr      regs[30]  
#define ARM_r0      regs[0]
#define PTRACE_GETREGS PTRACE_GETREGSET  
#define PTRACE_SETREGS PTRACE_SETREGSET 
#endif


#if defined(__aarch64__)
const char *libc_path = "/system/lib64/libc.so";
const char *linker_path = "/system/bin/linker64";
#else
const char *libc_path = "/system/lib/libc.so";
const char *linker_path = "/system/bin/linker";
#endif

#if ENABLE_DEBUG
	#define TAG "INJECT"
	#define	LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG,TAG,__VA_ARGS__)
	#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,TAG,__VA_ARGS__)
	#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR,TAG,__VA_ARGS__)
#else
	#define LOGD(...)
	#define LOGI(...)
	#define LOGE(...)
#endif

int ptrace_readdata(pid_t pid, uint8_t *src, uint8_t *buf, size_t size)
{
	uint32_t i, j, remain;
	uint8_t *laddr;
	size_t align = sizeof(long);

	union u {
		long val;
		char chars[sizeof(long)];
	} d;

	j = size / align;
	remain = size % align;

	laddr = buf;

	for ( i = 0; i < j; i++ ) {
		d.val = (long)ptrace(PTRACE_PEEKTEXT, pid, src, 0);
		memcpy(laddr, d.chars, align);
		src += align;
		laddr += align;
	}

	if ( remain > 0 ) {
		d.val = (long)ptrace(PTRACE_PEEKTEXT, pid, src, 0);
		memcpy(laddr, d.chars, remain);
	}

	return 0;
}

int ptrace_writedata(pid_t pid, uint8_t *dest, uint8_t *data, size_t size)
{
	uint32_t i, j, remain;
	uint8_t *laddr;
	size_t align = sizeof(long);

	union u {
		long val;
		char chars[sizeof(long)];
	} d;

	j = size / align;
	remain = size % align;

	laddr = data;

	for ( i = 0; i < j; i++ ) {
		memcpy(d.chars, laddr, align);
		ptrace(PTRACE_POKETEXT, pid, dest, d.val);
		dest += align;
		laddr += align;
	}

	if ( remain > 0 ) {
		//d.val = 0;
		//memcpy(d.chars, laddr, remain);
		//采用下面这种方式不会修改后面几个字节的数据
		d.val = (long)ptrace(PTRACE_PEEKTEXT, pid, dest, 0);
		for ( i = 0; i < remain; i++ ) {
			d.chars[i] = *laddr++;
		}
		ptrace(PTRACE_POKETEXT, pid, dest, d.val);
	}

	return 0;
}

int ptrace_writestring(pid_t pid, uint8_t *dest, const char *str)
{
	return ptrace_writedata(pid, dest, (uint8_t *)str, strlen(str)+1);
}


int ptrace_getregs(pid_t pid, struct pt_regs *regs)
{
#if defined(__aarch64__)

	struct iovec ioVec;
	ioVec.iov_base = regs;
	ioVec.iov_len = sizeof(*regs);
	if (ptrace(PTRACE_GETREGS, pid, NT_PRSTATUS, &ioVec) < 0) {
		perror("ptrace_getregs: Can not get register values");
		return -1;
	}

#else

	if (ptrace(PTRACE_GETREGS, pid, NULL, regs) < 0) {
		perror("ptrace_getregs: Can not get register values");
		return -1;
	}

#endif

	return 0;
}

int ptrace_setregs(pid_t pid, struct pt_regs *regs)
{
#if defined(__aarch64__)
	
	struct iovec ioVec;
	ioVec.iov_base = regs;
	ioVec.iov_len = sizeof(*regs);
	if (ptrace(PTRACE_SETREGS, pid, NT_PRSTATUS, &ioVec) < 0) {
		perror("ptrace_setregs: Can not set register values");
		return -1;
	}

#else

	if (ptrace(PTRACE_SETREGS, pid, NULL, regs) < 0) {
		perror("ptrace_setregs: Can not set register values");
		return -1;
	}
	return 0;

#endif

	return 0;
}

int ptrace_continue(pid_t pid)
{
	if (ptrace(PTRACE_CONT, pid, NULL, 0) < 0) {
		perror("ptrace_continue");
		return -1;
	}
	return 0;
}

int ptrace_attach(pid_t pid)
{
	int stat = 0;

	if (ptrace(PTRACE_ATTACH, pid, NULL, 0) < 0) {
		perror("ptrace_attach");
		return -1;
	}

#if 1
	waitpid(pid, &stat, WUNTRACED);
#else
	if (ptrace(PTRACE_SYSCALL, pid, NULL, 0) < 0) {
		perror("ptrace_syscall");
		return -1;
	}

	waitpid(pid, NULL, WUNTRACED);
#endif

	return 0;
}

int ptrace_detach(pid_t pid)
{
	if (ptrace(PTRACE_DETACH, pid, NULL, 0) < 0) {
		perror("ptrace_detach");
		return -1;
	}
	return 0;
}

#if defined(__arm__) || defined(__aarch64__)

int ptrace_call(pid_t pid, uintptr_t addr, long *params, uint32_t params_num, struct pt_regs *regs)
{
	uint32_t i;
	int stat = 0;

#if defined(__arm__)
	int params_num_registers = 4;
#else //__aarch64__
	int params_num_registers = 8;
#endif

	for (i = 0; i < params_num && i < params_num_registers; i++) {
		regs->uregs[i] = params[i];
	}

	if ( i < params_num ) {
		regs->ARM_sp -= (params_num - i) * sizeof(long);
		ptrace_writedata(pid, (uint8_t *)regs->ARM_sp, (uint8_t *)&params[i], (params_num - i) * sizeof(long));
	}

	regs->ARM_pc = addr;
	if ( regs->ARM_pc & 1 ) {
		/* thumb
		判断最后一位:
			1是thumb指令集
			0是arm指令集
		*/
		regs->ARM_pc &= (~1u);
		regs->ARM_cpsr |= CPSR_T_MASK;
	} else {
		/* arm */	
		regs->ARM_cpsr &= ~CPSR_T_MASK;
	}

	regs->ARM_lr = 0;

	if (ptrace_setregs(pid, regs) == -1 ||
			ptrace_continue(pid) == -1) {
		printf("ptrace_setregs or ptrace_continue error.");
		return -1;
	}

	waitpid(pid, &stat, WUNTRACED);
	while ( stat != 0xb7f ) {
		if (ptrace_continue(pid) == -1) {
			printf("ptrace_continue error.");
			return -1;
		}
		waitpid(pid, &stat, WUNTRACED);
	}

	return 0;
}

#elif defined(__i386__)

int ptrace_call(pid_t pid, uintptr_t addr, long *params, uint32_t params_num, struct pt_regs *regs)
{
	int stat = 0;

	regs->esp -= (params_num) * sizeof(long);
	ptrace_writedata(pid, (uint8_t *)regs->esp, (uint8_t *)&params, (params_num ) * sizeof(long));

	regs->eip = addr;

	if (ptrace_setregs(pid, regs) == -1 ||
			ptrace_continue(pid) == -1) {
		printf("ptrace_setregs or ptrace_continue error.");
		return -1;
	}

	waitpid(pid, &stat, WUNTRACED);
	while ( stat != 0xb7f ) {
		if (ptrace_continue(pid) == -1) {
			printf("ptrace_continue error.");
			return -1;
		}
		waitpid(pid, &stat, WUNTRACED);
	}

	return 0;
}

#else
	
	#error "ptrace_call not supported"

#endif

long ptrace_retval(struct pt_regs *regs)
{
#if defined(__arm__) || defined(__aarch64__)
	return regs->ARM_r0;
#elif defined(__i386__)
	return regs->eax;
#else
	#error "ptrace_retval not supported";
#endif
}

long ptrace_ip(struct pt_regs *regs)
{
#if defined(__arm__) || defined(__aarch64__)
	return regs->ARM_pc;
#elif defined(__i386__)
	return regs->eip;
#else
	#error "ptrace_ip not supported";
#endif
}

int ptrace_call_wrapper(pid_t target_pid, const char *func_name, void *func_addr, long *params, int param_num, struct pt_regs *regs)
{
	printf("[+]Calling %s in target process.\n", func_name);

	if (ptrace_call(target_pid, (uintptr_t)func_addr, params, param_num, regs) == -1)
		return -1;		
	if (ptrace_getregs(target_pid, regs) == -1)
		return -1;

#if defined(__aarch64__)
	printf("[+]return value=%lx, pc=%lx\n", (uintptr_t)ptrace_retval(regs), (uintptr_t)ptrace_ip(regs));
#else
	printf("[+]return value=%x, pc=%x\n", (uintptr_t)ptrace_retval(regs), (uintptr_t)ptrace_ip(regs));
#endif

	return 0;
}

void* get_module_base(pid_t pid, const char* module_name)
{
	FILE *fp;
	long addr = 0;
	char *pch;
	char filename[32];
	char line[1024];

	if ( pid < 0 )
		snprintf(filename, sizeof(filename), "/proc/self/maps");
	else
		snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);


	fp = fopen(filename, "r");
	if ( fp != NULL ) {
		while ( fgets(line, sizeof(line), fp) ) {
			if (strstr(line, module_name)) {
				pch = strtok(line, "-");
				addr = strtoul(pch, NULL, 16);

				if ( addr == 0x8000 )
					addr = 0;

				break;
			}
		}
		fclose(fp);
	} else {
		printf("open file %s failed. \n", filename);
	}

	return (void *)addr;
}

void* get_remote_addr(pid_t target_id, const char* module_name, void* local_addr)
{
	void *local_handle, *remote_handle, *ret_addr;

	local_handle = get_module_base(-1, module_name);
	remote_handle = get_module_base(target_id, module_name);

	//printf("[+] get_remote_addr: local[%x], remote[%x]\n", (uint32_t)local_handle, (uint32_t)remote_handle);

	ret_addr = (void *)((uintptr_t)local_addr + (uintptr_t)remote_handle - (uintptr_t)local_handle);

#if defined(__i386__)
	if (!strcmp(module_name, libc_path)) {
		ret_addr += 2;
	}
#endif

	return ret_addr;
}

int ptrace_call_mmap(pid_t target_pid, struct pt_regs *regs, long *params, long**retval)
{
	static void *mmap_addr = NULL;

	if (mmap_addr == NULL) {
		mmap_addr = get_remote_addr(target_pid, libc_path, (void *)mmap);
		if (mmap_addr == NULL)
			return -1;
	}

	if (ptrace_call_wrapper(target_pid, "mmap", mmap_addr, params, 6, regs))
		return -1;

	*retval = (long *)ptrace_retval(regs);

	return 0;
}
/*
int ptrace_call_dlerror(pid_t target_pid, void *func_addr, struct pt_regs *regs) 
{
	void *error_base;
	char line[1024];

	if (ptrace_call_wrapper(target_pid, "dlerror", func_addr, NULL, 0, &regs))
		return -1;

	error_base = (void *)ptrace_retval(&regs);

	ptrace_readdata(target_pid, (uint8_t *)error_base, (uint8_t *)line, strlen(line));
	printf("[+]ptrace_call_dlerror. %s\n", line);
}
*/

int find_pid_of(const char *process_name)
{
	int id;
	pid_t pid = -1;
	DIR *dir;
	FILE *fp;
	char filename[32];
	char cmdline[256];
	struct dirent *entry;

	if ( process_name == NULL)
		return -1;

	dir = opendir("/proc");
	if ( dir == NULL )
		return -1;

	while ( (entry = readdir(dir)) != NULL ) {
		id = atoi(entry->d_name);
		if ( id != 0 ) {
			sprintf(filename, "/proc/%d/cmdline", id);
			fp = fopen(filename, "r");
			if ( fp ) {
				fgets(cmdline, sizeof(cmdline), fp);
				fclose(fp);

				if ( strcmp(process_name, cmdline) == 0 ) {
					pid = id;
					break;
				}
			}
		}
	}

	closedir(dir);

	return pid;
}

int print_remote_string(pid_t pid, uint8_t *src)
{
	char line[1024];

	if (ptrace_readdata(pid, src, (uint8_t *)line, sizeof(line)))
		return -1;

	printf("ptrace_readdatastring: %s\n", line);

	return 0;
}


int inject_remote_process(pid_t target_pid, const char *library_path, const char *func_name, void *param, size_t param_size)
{
#define FUNCTION_NAME_ADDR_OFFSET 0x100
#define FUNCTION_PARAM_ADDR_OFFSET 0x200

	void *mmap_addr, *dlopen_addr, *dlsym_addr, *dlclose_addr, *dlerror_addr;
	void *mmap_base, *so_handle, *hook_entry_addr;
	struct pt_regs regs, original_regs;

	long params[10];

	printf("[+] Injecting process: %d\n", target_pid);

	if (ptrace_attach(target_pid) == -1)
		return -1;

	if (ptrace_getregs(target_pid, &regs) == -1)
		return -1;

	memcpy(&original_regs, &regs, sizeof(regs));

	mmap_addr = get_remote_addr(target_pid, libc_path, (void *)mmap);
	
	printf( "[+] from library %s get func addr, mmap: %x\n", libc_path, (uint32_t)mmap_addr);

	//printf("[+] Calling mmap in target process.\n");
	params[0] = 0;	//addr
	params[1] = 0x4000; //size
	params[2] = PROT_READ | PROT_WRITE | PROT_EXEC;	//prot
	params[3] = MAP_ANONYMOUS | MAP_PRIVATE; //flags
	params[4] = 0; //fd
	params[5] = 0; //offset

	if (ptrace_call_wrapper(target_pid, "mmap", mmap_addr, params, 6, &regs) == -1)
		return -1;

	mmap_base = (void *)ptrace_retval(&regs);
	//printf("mmap_base=%x\n", (uint32_t)mmap_base);
	if (mmap_base == NULL)
		return -1;

	dlopen_addr = get_remote_addr(target_pid, linker_path, (void *)dlopen);
	dlsym_addr = get_remote_addr(target_pid, linker_path, (void *)dlsym);
	dlclose_addr = get_remote_addr(target_pid, linker_path, (void *)dlclose);
	dlerror_addr = get_remote_addr(target_pid, linker_path, (void *)dlerror);

	printf( "[+] from library %s get func addr, dlopen: %x, dlsym: %x, dlclose: %x,dlerror: %x\n", 
		linker_path, (uint32_t)dlopen_addr, (uint32_t)dlsym_addr, (uint32_t)dlclose_addr, (uint32_t)dlerror_addr);

	printf("[+]Write library path in target process. %s\n", library_path);
	ptrace_writestring(target_pid, mmap_base, library_path);

	print_remote_string(target_pid, (uint8_t *)(mmap_base));

	//printf("[+] Calling dlopen in target process.\n");
	params[0] = (long)mmap_base;
	params[1] = RTLD_NOW | RTLD_GLOBAL;
	if (ptrace_call_wrapper(target_pid, "dlopen", dlopen_addr, params, 2, &regs) == -1)
		return -1;

	so_handle = (void *)ptrace_retval(&regs);
	//printf("so_handle=%x\n", (uint32_t)so_handle);
	if (so_handle == NULL)
		return -1;

	printf("[+]Write function name in target process. %s\n", func_name);
	ptrace_writestring(target_pid, mmap_base+FUNCTION_NAME_ADDR_OFFSET, func_name);

	print_remote_string(target_pid, (uint8_t *)(mmap_base+FUNCTION_NAME_ADDR_OFFSET));

	//printf("[+] Calling dlsym in target process.\n");
	params[0] = (long)so_handle;
	params[1] = (long)(mmap_base+FUNCTION_NAME_ADDR_OFFSET);
	if (ptrace_call_wrapper(target_pid, "dlsym", dlsym_addr, params, 2, &regs) == -1)
		return -1;

	hook_entry_addr = (void *)ptrace_retval(&regs);
	//printf("hook_entry_addr=%x\n", (uint32_t)so_handle);
	if (hook_entry_addr == NULL)
		return -1;

	printf("[+]Write params in target process.\n");
	ptrace_writestring(target_pid, mmap_base+FUNCTION_PARAM_ADDR_OFFSET, (const char *)param);

	//printf("[+] Calling hook_entry in target process.\n");
	params[0] = (long)(mmap_base+FUNCTION_PARAM_ADDR_OFFSET);
	if (ptrace_call_wrapper(target_pid, "hook_entry", hook_entry_addr, params, 1, &regs) == -1)
		return -1;

	//Enter 后会detach测试程序才会运行起来，否则处于暂停状态
	printf("Press enter to dlclose and detach\n");
	getchar();

#if 0
	//关闭后inject的so会被卸载 hook则失效
	//printf("[+] Calling dlclose in target process.\n");
	params[0] = (long)so_handle;
	if (ptrace_call_wrapper(target_pid, "dlclose", dlclose_addr, params, 1, &regs) == -1)
		return -1;
#endif

	ptrace_setregs(target_pid, &original_regs);
	ptrace_detach(target_pid);

	return 0;
}


void print_usage(char** argv) {
	fprintf(stderr, "error usage: %s -p PID [-P PROCNAME] -l LIBNAME [-f FUNCTION] [-s FUNCPARAM]\n", argv[0]);
}

void print_arg(const char *arg_name, const char *arg_val) {
#if 1
	printf("%s=%s\n", arg_name, arg_val);
#endif
}

int main(int argc, char** argv) 
{
#if 1
	pid_t target_pid = -1;
	char *proc_name = NULL;
	char *lib_path = NULL;
	char *func_name = "hook_entry";
	char *func_params = "";

	int opt;

	while ((opt = getopt(argc, argv, "p:P:l:f:s:")) != -1) {
		switch ( opt ) {
			case 'p':
				target_pid = strtol(optarg, NULL, 0);
				break;
			case 'P':
				proc_name = strdup(optarg);
				print_arg("proc_name", proc_name);
				break;
			case 'l':
				lib_path = strdup(optarg);
				print_arg("lib_path", lib_path);
				break;
			case 'f':
				func_name = strdup(optarg);
				print_arg("func_name", func_name);
				break;
			case 's':
				func_params = strdup(optarg);
				print_arg("func_params", func_params);
				break;
			default:
				print_usage(argv);
				exit(0);
		}
	}

	if (proc_name != NULL && target_pid < 0)
		target_pid = find_pid_of(proc_name);

	if (target_pid <= 0 || lib_path == NULL)  {
		print_usage(argv);
		exit(0);
	}

	inject_remote_process(target_pid, lib_path, func_name, func_params, strlen(func_params));

#else
	char *host_process, *inject_lib, *func_name, *func_params;
	pid_t target_pid;

	printf("params count is %d\n", argc);
	for (int i = 0; i < argc; ++i)
	{
		printf("params[%d]=%s\n", i, argv[i]);
	}

	if (argc < 3)
	{
		printf("inject host_process inject_lib func_name func_params\n");
		return -1;
	}

	if (argc > 1)
	{
		host_process = argv[1];
		printf("Host process is %s\n", host_process);
	}
	
	if (argc > 2)
	{
		inject_lib = argv[2];
		printf("Inject lib is %s\n", inject_lib);
	}
	
	if (argc > 3)
		func_name = argv[3];
	else
		func_name = "hook_entry";
	printf("Function name is %s\n", func_name);

	if (argc > 4)
		func_params = argv[4];
	else
		func_params = "hello inject.";
	printf("Function params is %s\n", func_params);


	target_pid = find_pid_of(host_process);
	if (target_pid <= 0)
	{
		printf("not found process id.\n");
		return -1;
	}

	inject_remote_process(target_pid, inject_lib, func_name, func_params, strlen(func_params));
#endif

	return 0;
}