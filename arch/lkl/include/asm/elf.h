#ifndef _ASM_LKL_ELF_H
#define _ASM_LKL_ELF_H

#define elf_check_arch(x) 0

#ifdef CONFIG_64BIT
#define ELF_CLASS ELFCLASS64
#else
#define ELF_CLASS ELFCLASS32
#endif

#ifdef CONFIG_CPU_BIG_ENDIAN
#define ELF_DATA ELFDATA2MSB
#else
#define ELF_DATA ELFDATA2LSB
#endif

#if defined(__x86_64__)
#define ELF_ARCH EM_X86_64
#elif defined(__i386__)
#define ELF_ARCH EM_386
#elif defined(__aarch64__)
#define ELF_ARCH EM_AARCH64
#elif defined(__arm__)
#define ELF_ARCH EM_ARM
#elif defined(__riscv)
#define ELF_ARCH EM_RISCV
#elif defined(__s390__)
#define ELF_ARCH EM_S390
#else
#error "Unsupported LKL ELF target"
#endif

#ifdef CONFIG_MMU
#define ELF_EXEC_PAGESIZE 4096
#define ELF_PLATFORM "i586"
#define ELF_HWCAP 0L
#define ELF_ET_DYN_BASE (TASK_SIZE)
#endif // CONFIG_MMU

#define elf_gregset_t long
#define elf_fpregset_t double
#endif
