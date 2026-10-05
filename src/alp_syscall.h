/*
 * alp_syscall.h - names for the Linux system calls and flags ALP Snake uses.
 *
 * The game is built "freestanding": it links against no C library at all,
 * because the cabinet's Retroplayer loads our core as a plain shared object
 * and we do not want to depend on whichever libc version the firmware ships.
 * Instead of open()/read()/write() we ask the kernel directly with the
 * aarch64 "svc #0" instruction (see the system_call() wrappers in
 * snake_libretro.c and snake_backglass.c, and font_call() in
 * snake_smooth_font.c).
 *
 * On aarch64 Linux the system call number goes in register x8 and up to six
 * arguments go in x0..x5. The numbers below come from the kernel's generic
 * syscall table (include/uapi/asm-generic/unistd.h), which arm64 uses.
 * Note there is no plain open(), rename() or unlink() on arm64: you use the
 * "...at" versions with AT_FDCWD, meaning "relative to the current folder".
 *
 * Careful: a few flag values are different on arm64 than on x86. For example
 * O_DIRECTORY is 0x4000 here (it is 0x10000 on x86, which on arm64 means
 * O_DIRECT). Always check flags against the arm64 headers, not a PC's.
 *
 * Each name is guarded with #ifndef so the host preview builds, which do
 * include the system's own headers, still compile. (Those builds never make
 * real system calls, so the host's values do not matter there.)
 */
#ifndef ALP_SYSCALL_H
#define ALP_SYSCALL_H

/* System call numbers (arm64 / asm-generic table). */
#define SYS_ioctl          29   /* device control; used to query the trackball */
#define SYS_unlinkat       35   /* delete a file */
#define SYS_renameat       38   /* atomically replace one file with another */
#define SYS_openat         56   /* open a file, returns a descriptor */
#define SYS_close          57
#define SYS_getdents64     61   /* list a directory (we list /proc/self/fd) */
#define SYS_read           63
#define SYS_write          64
#define SYS_fsync          82   /* flush a file to the flash before renaming it */
#define SYS_exit           93   /* end this thread only (see snake_backglass.c) */
#define SYS_nanosleep     101
#define SYS_clock_gettime 113   /* the monotonic clock behind all game timing */
#define SYS_kill          129
#define SYS_prctl         167
#define SYS_getpid        172
#define SYS_getppid       173
#define SYS_clone         220   /* fork-style process creation */
#define SYS_execve        221   /* replace the child with the backglass helper */
#define SYS_wait4         260   /* collect a finished child process */

/* "Relative to the current working directory" for the ...at() calls. */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

/* openat() flags, arm64 values. */
#ifndef O_RDONLY
#define O_RDONLY    0x0
#endif
#ifndef O_WRONLY
#define O_WRONLY    0x1
#endif
#ifndef O_CREAT
#define O_CREAT     0x40
#endif
#ifndef O_TRUNC
#define O_TRUNC     0x200
#endif
#ifndef O_APPEND
#define O_APPEND    0x400
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK  0x800      /* read() returns at once if nothing is waiting */
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0x4000     /* arm64 value; x86 uses 0x10000 */
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC   0x80000    /* do not leak this descriptor into execve() */
#endif

/* clock_gettime() clock: never jumps when the wall-clock time is changed. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* Signals and process control. */
#ifndef SIGTERM
#define SIGTERM 15
#endif
#ifndef SIGCHLD
#define SIGCHLD 17             /* clone() flag: tell the parent when we exit */
#endif
#ifndef PR_SET_PDEATHSIG
#define PR_SET_PDEATHSIG 1     /* prctl(): send us a signal when the parent dies */
#endif
#ifndef WNOHANG
#define WNOHANG 1              /* wait4(): return at once if the child still runs */
#endif

/* ioctl(EVIOCGBIT(EV_REL, 8)): which relative axes (REL_X, REL_Y...) an input
 * device reports. The trackball is the device that reports REL_X and REL_Y. */
#define ALP_EVIOCGBIT_REL 0x80084522

#endif
