/* Simple command-line kernel monitor useful for
 * controlling the kernel and exploring the system interactively. */

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/env.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/env.h>
#include <kern/trap.h>
#include <kern/kclock.h>

#define WHITESPACE "\t\r\n "
#define MAXARGS    16

/* Functions implementing monitor commands */
int mon_help(int argc, char **argv, struct Trapframe *tf);
int mon_kerninfo(int argc, char **argv, struct Trapframe *tf);
int mon_backtrace(int argc, char **argv, struct Trapframe *tf);
int mon_dumpcmos(int argc, char **argv, struct Trapframe *tf);

struct Command {
    const char *name;
    const char *desc;
    /* return -1 to force monitor to exit */
    int (*func)(int argc, char **argv, struct Trapframe *tf);
};

static struct Command commands[] = {
        {"help", "Display this list of commands", mon_help},
        {"kerninfo", "Display information about the kernel", mon_kerninfo},
        {"backtrace", "Print stack backtrace", mon_backtrace},
        {"dumpcmos", "Print CMOS contents", mon_dumpcmos},
};
#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

/* Implementations of basic kernel monitor commands */

int
mon_help(int argc, char **argv, struct Trapframe *tf) {
    for (size_t i = 0; i < NCOMMANDS; i++)
        cprintf("%s - %s\n", commands[i].name, commands[i].desc);
    return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf) {
    extern char _head64[], entry[], etext[], edata[], end[];

    cprintf("Special kernel symbols:\n");
    cprintf("  _head64 %16lx (virt)  %16lx (phys)\n", (unsigned long)_head64, (unsigned long)_head64);
    cprintf("  entry   %16lx (virt)  %16lx (phys)\n", (unsigned long)entry, (unsigned long)entry - KERN_BASE_ADDR);
    cprintf("  etext   %16lx (virt)  %16lx (phys)\n", (unsigned long)etext, (unsigned long)etext - KERN_BASE_ADDR);
    cprintf("  edata   %16lx (virt)  %16lx (phys)\n", (unsigned long)edata, (unsigned long)edata - KERN_BASE_ADDR);
    cprintf("  end     %16lx (virt)  %16lx (phys)\n", (unsigned long)end, (unsigned long)end - KERN_BASE_ADDR);
    cprintf("Kernel executable memory footprint: %luKB\n", (unsigned long)ROUNDUP(end - entry, 1024) / 1024);
    return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf) {
    // LAB 2: Your code here
    cprintf("Stack backtrace:\n");
    //uint64_t test;
    //cprintf("test %016lx\n", (long unsigned int)(&test+3));
    struct Ripdebuginfo info;
    uint64_t *rbp = (uint64_t *)read_rbp();
    uint64_t rip;
    while (rbp) {
        rip = *(rbp + 1);
        debuginfo_rip(rip, &info);
        cprintf("  rbp %016lx rip %016lx\n", (long unsigned int)rbp, (long unsigned int)rip);
        cprintf("    %.s:%d: %.*s+%ld\n", info.rip_file, 
                                          info.rip_line, 
                                          info.rip_fn_namelen, 
                                          info.rip_fn_name, 
                                          (rip - info.rip_fn_addr));
        rbp = (uint64_t *)(*rbp);
    };
    return 0;
}

int
mon_dumpcmos(int argc, char **argv, struct Trapframe *tf) {
    // Dump CMOS memory in the following format:
    // 00: 00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF
    // 10: 00 ..
    // Make sure you understand the values read.
    // Hint: Use cmos_read8()/cmos_write8() functions.
    // LAB 4: Your code here
    size_t offset = 0x0;
    for (size_t i = CMOS_START; i < CMOS_SIZE + CMOS_START; ++i) {
        if (offset % 0x10 == 0) {
            if (offset) cprintf("\n");
            cprintf("%02lx: ", offset);
        }
        cprintf("%02x ", cmos_read8(i));
        offset++;
    }
    cprintf("\n");
    return 0;
}

/* Kernel monitor command interpreter */

static int
runcmd(char *buf, struct Trapframe *tf) {
    int argc = 0;
    char *argv[MAXARGS];

    argv[0] = NULL;

    /* Parse the command buffer into whitespace-separated arguments */
    for (;;) {
        /* gobble whitespace */
        while (*buf && strchr(WHITESPACE, *buf)) *buf++ = 0;
        if (!*buf) break;

        /* save and scan past next arg */
        if (argc == MAXARGS - 1) {
            cprintf("Too many arguments (max %d)\n", MAXARGS);
            return 0;
        }
        argv[argc++] = buf;
        while (*buf && !strchr(WHITESPACE, *buf)) buf++;
    }
    argv[argc] = NULL;

    /* Lookup and invoke the command */
    if (!argc) return 0;
    for (size_t i = 0; i < NCOMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0)
            return commands[i].func(argc, argv, tf);
    }

    cprintf("Unknown command '%s'\n", argv[0]);
    return 0;
}

void
monitor(struct Trapframe *tf) {

    cprintf("Welcome to the JOS kernel monitor!\n");
    cprintf("Type 'help' for a list of commands.\n");

    if (tf) print_trapframe(tf);
    char *buf;
    do buf = readline("K> ");
    while (!buf || runcmd(buf, tf) >= 0);
}