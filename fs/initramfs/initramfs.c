/*
 * Maintainer: Cristopher Borjas <borjascristopher88@gmail.com>
 * Source: kernel/fs/initramfs/initramfs.c
 * Description: initramfs: the boot-time root filesystem. In this phase the kernel
 * Added: 2026-10-17
 */
#include <fs/vfs.h>
#include <core/core_printk.h>
#include <core/core_version.h>
#include <fs/tmpfs.h>
#include <iru_string.h>

/* initramfs: the boot-time root filesystem. In this phase the kernel
 * populates a tmpfs-backed root from built-in definitions (including the
 * embedded /bin/hello + /init images). Later this module parses a cpio
 * archive passed by the boot loader as a multiboot module. */

#define DEFAULT_ROOT_DIRS \
    { "boot", "dev", "proc", "sys", "tmp", "etc", "bin", "sbin", \
      "lib", "lib64", "usr", "home", "root", "run", "opt", "var", "mnt" }

static const char *root_dirs[] = DEFAULT_ROOT_DIRS;

/* Embedded userland binaries (linked by the build system). Symbol renames
 * to _binary_<name>_elf_*. */
extern const u8 _binary_hello_elf_start[];
extern const u8 _binary_hello_elf_end[];
extern const u8 _binary_init_elf_start[];
extern const u8 _binary_init_elf_end[];

/* Embedded test images for the img viewer (splash.bmp, test.ppm). */
extern const u8 _binary_splash_bmp_start[];
extern const u8 _binary_splash_bmp_end[];
extern const u8 _binary_test_ppm_start[];
extern const u8 _binary_test_ppm_end[];

/* Desktop wallpaper plate (brand/newos-wallpaper.bmp). */
extern const u8 _binary_wallpaper_bmp_start[];
extern const u8 _binary_wallpaper_bmp_end[];

#define DECL_BIN(n)                               \
    extern const u8 _binary_##n##_elf_start[];    \
    extern const u8 _binary_##n##_elf_end[];

DECL_BIN(ls)
DECL_BIN(cat)
DECL_BIN(echo)
DECL_BIN(img)
DECL_BIN(vid)
DECL_BIN(about)
DECL_BIN(newfetch)
DECL_BIN(mkdir)
DECL_BIN(rmdir)
DECL_BIN(rm)
DECL_BIN(mv)
DECL_BIN(touch)
DECL_BIN(pwd)
DECL_BIN(clear)
DECL_BIN(uname)
DECL_BIN(hostname)
DECL_BIN(whoami)
DECL_BIN(id)
DECL_BIN(date)
DECL_BIN(uptime)
DECL_BIN(ps)
DECL_BIN(kill)
DECL_BIN(sleep)
DECL_BIN(free)
DECL_BIN(df)
DECL_BIN(wc)
DECL_BIN(head)
DECL_BIN(grep)
DECL_BIN(find)
DECL_BIN(sort)
DECL_BIN(newpkg)
DECL_BIN(frotz)
DECL_BIN(doom)
DECL_BIN(busybox)
DECL_BIN(glxgears)

/* The NEWOS Win95-style desktop session (installed as /bin/desktop and
 * started by the OpenRC `desktop` service; see /etc/conf.d/desktop). */
DECL_BIN(desktop)

/* Host-built static Linux/musl programs from the vendored ports. Both run
 * through the Linux-compatible SYSCALL entry (see ports/tcc and ports/nano). */
DECL_BIN(nano)
DECL_BIN(tcc)

/* VOXEL classic desktop (MIT, ports/voxel): NEWOS-native ELF built from
 * source by scripts/build-voxel.sh. */
DECL_BIN(voxel)

/* Host-built GNU tools, packed by scripts/pack-gnu.sh into one `.new`
 * archive and staged at /tmp for the shell's startup install. */
extern const u8 _binary_gnu_coreutils_new_start[];
extern const u8 _binary_gnu_coreutils_new_end[];

/* NEWOS C SDK (headers + libc.a + crt0.o) for the on-board tcc; staged at
 * /tmp and installed with `newpkg install /tmp/newos-sdk.new`. */
extern const u8 _binary_newos_sdk_new_start[];
extern const u8 _binary_newos_sdk_new_end[];

/* Sample `.new` package embedded by the build (tools/newpkg): staged as
 * `sample.new` -> _binary_sample_new_{start,end}. */
extern const u8 _binary_sample_new_start[];
extern const u8 _binary_sample_new_end[];

/* Demo Z-code story for /bin/frotz: tools/frotz/tests/mini.z3 ->
 * _binary_mini_z3_{start,end}, installed at /usr/share/zcode/mini.z3. */
extern const u8 _binary_mini_z3_start[];
extern const u8 _binary_mini_z3_end[];

/* DOOM shareware IWAD for /bin/doom: user/programs/doom/data/doom1.wad ->
 * _binary_doom1_wad_{start,end}, installed at /usr/share/doom/doom1.wad
 * (the directory /bin/doom's IWAD search checks first). */
extern const u8 _binary_doom1_wad_start[];
extern const u8 _binary_doom1_wad_end[];

/* Shell boot script: user/rootfs/nsh.rc -> _binary_nsh_rc_{start,end}. */
extern const u8 _binary_nsh_rc_start[];
extern const u8 _binary_nsh_rc_end[];

/* System nano configuration: user/rootfs/nanorc -> /usr/etc/nanorc. */
extern const u8 _binary_nanorc_start[];
extern const u8 _binary_nanorc_end[];

/* OpenRC service manager: /sbin/openrc is the runlevel engine,
 * /sbin/openrc-init the PID 1 that supervises the console, and the rc-*
 * tools are its operators. Built from user/programs/openrc. */
DECL_BIN(openrc)
DECL_BIN(openrc_init)
DECL_BIN(rc_status)
DECL_BIN(rc_update)
DECL_BIN(rc_service)

/* OpenRC configuration tree (/etc/rc.conf, /etc/inittab, /etc/conf.d,
 * /etc/init.d, /etc/runlevels) packed by scripts/pack-openrc.py. */
extern const u8 _binary_openrc_rootfs_orcf_start[];
extern const u8 _binary_openrc_rootfs_orcf_end[];

static const struct {
    const char *name;
    const u8 *start;
    const u8 *end;
} bin_table[] = {
#define BIN(n) { #n, _binary_##n##_elf_start, _binary_##n##_elf_end },
    BIN(ls)
    BIN(cat)
    BIN(echo)
    BIN(img)
    BIN(vid)
    BIN(about)
    BIN(newfetch)
    BIN(mkdir)
    BIN(rmdir)
    BIN(rm)
    BIN(mv)
    BIN(touch)
    BIN(pwd)
    BIN(clear)
    BIN(uname)
    BIN(hostname)
    BIN(whoami)
    BIN(id)
    BIN(date)
    BIN(uptime)
    BIN(ps)
    BIN(kill)
    BIN(sleep)
    BIN(free)
    BIN(df)
    BIN(wc)
    BIN(head)
    BIN(grep)
    BIN(find)
    BIN(sort)
    BIN(newpkg)
    BIN(frotz)
    BIN(doom)
    BIN(busybox)
    BIN(glxgears)
    BIN(nano)
    BIN(tcc)
    BIN(voxel)
    BIN(desktop)
};

/* /sbin: the system administration tools, where upstream OpenRC lives. The
 * VFS names keep upstream's hyphens, so the table is written out instead of
 * generated from the (underscore-only) blob symbols. */
static const struct {
    const char *name;
    const u8 *start;
    const u8 *end;
} sbin_table[] = {
    { "openrc", _binary_openrc_elf_start, _binary_openrc_elf_end },
    { "openrc-init", _binary_openrc_init_elf_start,
      _binary_openrc_init_elf_end },
    { "rc-status", _binary_rc_status_elf_start, _binary_rc_status_elf_end },
    { "rc-update", _binary_rc_update_elf_start, _binary_rc_update_elf_end },
    { "rc-service", _binary_rc_service_elf_start, _binary_rc_service_elf_end },
};

static void install_blob(const char *path, const u8 *data, size_t len,
                         struct vfs_inode *parent)
{
    const char *name = strrchr(path, '/');
    name = (name && name[1]) ? name + 1 : path;

    struct vfs_inode *node = tmpfs_create_node(parent, name, VFS_MODE_REG | 0755);
    struct vfs_file *f = vfs_open_inode(node, VFS_O_READ | VFS_O_WRITE);
    if (f) {
        vfs_write(f, data, len);
        vfs_close(f);
    }
}

/* OpenRC configuration tree (scripts/pack-openrc.py). The kernel installs
 * /etc/{rc.conf,inittab,conf.d,init.d,runlevels} from one blob so the service
 * manager finds a normal /etc on every boot - including a boot whose PID 1 is
 * plain nsh, where `openrc` and `rc-status` still have to work.
 *
 * Record layout: 'F' (file) or 'D' (directory) type byte, u32 path length,
 * u32 payload size, path bytes, payload. Every length is checked against the
 * blob before it is used, so a truncated or corrupt tail stops the walk
 * instead of writing outside the embedded image. */
static u32 orcf_u32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
           ((u32)p[3] << 24);
}

static void install_rc_tree(const u8 *blob, size_t len)
{
    size_t pos = 0, files = 0, dirs = 0;

    if (len < 4 || blob[0] != 'O' || blob[1] != 'R' || blob[2] != 'C' ||
        blob[3] != 'F') {
        pr_info("initramfs: no OpenRC configuration tree staged\n");
        return;
    }
    pos = 4;
    while (pos + 9 <= len) {
        u8 type = blob[pos];
        u32 pl = orcf_u32(blob + pos + 1);
        u32 sz = orcf_u32(blob + pos + 5);
        char path[512];
        struct vfs_inode *parent;

        pos += 9;
        if (pl == 0 || (size_t)pl >= sizeof(path) || pos + pl > len)
            break;
        for (u32 i = 0; i < pl; i++)
            path[i] = (char)blob[pos + i];
        path[pl] = '\0';
        if (path[0] != '/')
            break;
        pos += pl;
        if (pos + sz > len)
            break;
        if (type == 'D') {
            vfs_mkdir(path);
            dirs++;
        } else if (type == 'F') {
            /* install_blob() takes a parent node: resolve the directory. */
            char dir[512];
            size_t k = 0;

            while (path[k]) {
                dir[k] = path[k];
                k++;
            }
            dir[k] = '\0';
            while (k > 1 && dir[k - 1] != '/')
                k--;
            if (k > 1)
                k--;                      /* drop the separator itself */
            dir[k] = '\0';
            parent = vfs_lookup(dir);
            if (parent) {
                install_blob(path, blob + pos, (size_t)sz, parent);
                files++;
            } else {
                pr_warn("initramfs: rc tree: no parent for %s\n", path);
            }
        }
        pos += sz;
    }
    pr_info("initramfs: OpenRC tree: %zu files, %zu directories\n",
            files, dirs);
}

void initramfs_init(void)
{
    for (size_t i = 0; i < sizeof(root_dirs) / sizeof(root_dirs[0]); i++) {
        char path[128];
        strcpy(path, "/");
        strcat(path, root_dirs[i]);
        vfs_mkdir(path);
    }
    vfs_mkdir("/home/user");
    /* Conventional Linux-style hierarchy. Directories are created parent
     * first because this VFS intentionally does not synthesize missing ones. */
    static const char *system_dirs[] = {
        "/usr/bin", "/usr/sbin", "/usr/lib", "/usr/lib64", "/usr/include",
        "/usr/etc",
        "/usr/share", "/usr/share/doc", "/usr/share/doc/newos",
        "/usr/share/man", "/usr/share/misc", "/usr/share/licenses",
        "/usr/share/zcode", "/usr/share/doom", "/usr/src", "/usr/local",
        "/usr/local/bin", "/usr/local/sbin", "/usr/local/lib",
        "/usr/local/share", "/usr/local/etc", "/etc/skel", "/etc/init.d",
        "/etc/opt", "/run/lock", "/var/cache", "/var/cache/newpkg",
        "/var/lib", "/var/lib/misc", "/var/log", "/var/opt", "/var/spool",
        "/var/tmp", "/srv", "/media", "/home/user/.config",
        "/home/user/.cache", "/home/user/.local", "/home/user/.local/share",
        "/home/user/Desktop", "/home/user/Documents",
        "/home/user/Downloads", "/home/user/Pictures",
        "/root/Desktop", "/root/Documents", "/root/Downloads",
        "/root/Pictures"
    };
    for (size_t i = 0; i < sizeof(system_dirs) / sizeof(system_dirs[0]); i++)
        vfs_mkdir(system_dirs[i]);
    /* Package database home (userspace newpkg registers installs here). */
    vfs_mkdir("/var/lib");
    vfs_mkdir("/var/lib/newpkg");

    /* Classic dotfiles. */
    static const char hostname[] = "newos\n";
    static const char passwd[] =
        "root:x:0:0:root:/root:/bin/sh\n"
        "user:x:1000:1000:user:/home/user:/bin/sh\n";
    static const char group[] =
        "root:x:0:\n"
        "users:x:100:\n"
        "user:x:1000:user\n";
    static const char shells[] = "/bin/sh\n";
    static const char issue[] = "NEWOS " NEWOS_VERSION " \\n \\m (x86_64)\n\n";
    static const char profile[] =
        "# Per-user shell setup for NEWOS.\n"
        "# Built-in programs are in /bin; installed tools are in /usr/bin.\n";
    static const char motd[] =
        "\n"
        "  NEWOS " NEWOS_VERSION " (x86_64) - nsh shell\n"
        "  Type 'help' for commands.\n"
        "\n";
    static const char os_release[] =
        "NAME=\"NEWOS\"\n"
        "PRETTY_NAME=\"NEWOS " NEWOS_VERSION "\"\n"
        "ID=newos\n"
        "VERSION_ID=\"" NEWOS_VERSION_ID "\"\n"
        "VERSION_CODENAME=\"gui\"\n"
        "HOME_URL=\"https://github.com/CRISTOP-bot/NEWOS\"\n";
    static const char distro_readme[] =
        "NEWOS\n"
        "=====\n"
        "Native x86_64 operating system.\n"
        "\n"
        "System commands: /bin\n"
        "Additional packages: /usr/bin\n"
        "Configuration: /etc\n"
        "Variable state: /var\n";
    static const char root_readme[] =
        "This is root's home. Be careful, you are the superuser.\n";
    static const char user_readme[] =
        "Welcome home. Try: ls /bin -- cat /etc/motd -- help\n";
    /* Canonical copy: ports/tcc/demo/hello.c (keep the two in sync). */
    static const char tcc_demo[] =
        "/*\n"
        " * NEWOS tcc demo: a freestanding C program with no headers at all.\n"
        " *\n"
        " * The image installs this file as /etc/tcc-hello.c. TinyCC compiles and runs\n"
        " * it in place (no fork/exec: tcc -run relocates the code into this process\n"
        " * and calls main):\n"
        " *\n"
        " *     tcc -nostdlib -run /etc/tcc-hello.c\n"
        " *\n"
        " * The int $0x80 calls below are the native NEWOS ABI (rax = number,\n"
        " * rdi/rsi/rdx = arguments), not Linux's syscall instruction.\n"
        " */\n"
        "static long newos_syscall3(long nr, long a, long b, long c)\n"
        "{\n"
        "    long ret;\n"
        "\n"
        "    __asm__ volatile(\"int $0x80\"\n"
        "                     : \"=a\"(ret)\n"
        "                     : \"a\"(nr), \"D\"(a), \"S\"(b), \"d\"(c)\n"
        "                     : \"rcx\", \"r11\", \"memory\");\n"
        "    return ret;\n"
        "}\n"
        "\n"
        "static unsigned long text_len(const char *s)\n"
        "{\n"
        "    unsigned long n = 0;\n"
        "    while (s[n])\n"
        "        n++;\n"
        "    return n;\n"
        "}\n"
        "\n"
        "int main(void)\n"
        "{\n"
        "    const char msg[] = \"Hello from tcc on NEWOS!\\n\";\n"
        "\n"
        "    newos_syscall3(1, 1, (long)msg, (long)text_len(msg));  /* SYS_WRITE */\n"
        "    return 0;\n"
        "}\n";

    struct write_req {
        const char *path;
        const char *data;
    };
    static const struct write_req dotfiles[] = {
        { "/etc/hostname", hostname },
        { "/etc/passwd", passwd },
        { "/etc/group", group },
        { "/etc/shells", shells },
        { "/etc/issue", issue },
        { "/etc/motd", motd },
        { "/etc/os-release", os_release },
        { "/usr/share/doc/newos/README", distro_readme },
        { "/etc/skel/.profile", profile },
        { "/home/user/.profile", profile },
        { "/root/.profile", profile },
        { "/root/README", root_readme },
        { "/home/user/README", user_readme },
        { "/etc/tcc-hello.c", tcc_demo },
    };
    for (size_t i = 0; i < sizeof(dotfiles) / sizeof(dotfiles[0]); i++) {
        if (vfs_create(dotfiles[i].path) != 0)
            continue;
        struct vfs_file *f =
            vfs_open(dotfiles[i].path, VFS_O_READ | VFS_O_WRITE);
        if (f) {
            vfs_write(f, dotfiles[i].data, strlen(dotfiles[i].data));
            vfs_close(f);
        }
    }

    /* A welcome file so userspace has something to read. */
    vfs_create("/etc/version");
    struct vfs_file *f = vfs_open("/etc/version", VFS_O_READ | VFS_O_WRITE);
    if (f) {
        static const char banner[] = "NEWOS " NEWOS_VERSION "\n";
        vfs_write(f, banner, sizeof(banner) - 1);
        vfs_close(f);
    }

    /* Test pictures for the img viewer (generated at build time) and the
     * shell boot script. Empty blobs are skipped, so an asset the build
     * never produced simply does not appear in the image. */
    {
        struct {
            const char *path;
            const u8 *start;
            const u8 *end;
        } files[] = {
            { "/etc/splash.bmp", _binary_splash_bmp_start,
              _binary_splash_bmp_end },
            { "/etc/wallpaper.bmp", _binary_wallpaper_bmp_start,
              _binary_wallpaper_bmp_end },
            { "/etc/test.ppm", _binary_test_ppm_start,
              _binary_test_ppm_end },
            { "/etc/nsh.rc", _binary_nsh_rc_start,
              _binary_nsh_rc_end },
            { "/usr/etc/nanorc", _binary_nanorc_start,
              _binary_nanorc_end },
            { "/usr/share/zcode/mini.z3", _binary_mini_z3_start,
              _binary_mini_z3_end },
            { "/usr/share/doom/doom1.wad", _binary_doom1_wad_start,
              _binary_doom1_wad_end },
        };
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            size_t len = (size_t)(files[i].end - files[i].start);
            struct vfs_file *pf;
            if (!len)
                continue;
            if (vfs_create(files[i].path) != 0)
                continue;
            pf = vfs_open(files[i].path, VFS_O_READ | VFS_O_WRITE);
            if (!pf)
                continue;
            if (vfs_write(pf, files[i].start, len) != (int)len)
                pr_warn("initramfs: short write %s\n", files[i].path);
            vfs_close(pf);
            pr_info("initramfs: installed %s (%zu bytes)\n", files[i].path,
                    len);
        }
    }

    /* /bin/hello (phase-2 test program) and /init (the interactive shell). */
    size_t hello_len = (size_t)(_binary_hello_elf_end - _binary_hello_elf_start);
    size_t init_len = (size_t)(_binary_init_elf_end - _binary_init_elf_start);
    if (hello_len) {
        struct vfs_inode *bin = vfs_lookup("/bin");
        if (bin)
            install_blob("/bin/hello", _binary_hello_elf_start, hello_len, bin);
        pr_info("initramfs: installed /bin/hello (%zu bytes)\n", hello_len);
    }
    if (init_len) {
        struct vfs_inode *root = vfs_root();
        struct vfs_inode *bin = vfs_lookup("/bin");
        if (root)
            install_blob("/init", _binary_init_elf_start, init_len, root);
        if (bin)
            install_blob("/bin/sh", _binary_init_elf_start, init_len, bin);
        pr_info("initramfs: installed /init + /bin/sh (%zu bytes)\n",
                init_len);
    }

    /* The /bin toolbox: every userland command. */
    {
        struct vfs_inode *bin = vfs_lookup("/bin");
        size_t installed = 0;
        if (bin) {
            for (size_t i = 0;
                 i < sizeof(bin_table) / sizeof(bin_table[0]); i++) {
                size_t len =
                    (size_t)(bin_table[i].end - bin_table[i].start);
                if (!len)
                    continue;
                char path[80];
                strcpy(path, "/bin/");
                strcat(path, bin_table[i].name);
                install_blob(path, bin_table[i].start, len, bin);
                installed++;
            }
        }
        pr_info("initramfs: installed %zu /bin tools\n", installed);
    }

    /* /sbin: the service manager and its operators. */
    {
        struct vfs_inode *sbin = vfs_lookup("/sbin");
        size_t installed = 0;
        if (sbin) {
            for (size_t i = 0;
                 i < sizeof(sbin_table) / sizeof(sbin_table[0]); i++) {
                size_t len =
                    (size_t)(sbin_table[i].end - sbin_table[i].start);
                if (!len)
                    continue;
                char path[80];
                strcpy(path, "/sbin/");
                strcat(path, sbin_table[i].name);
                install_blob(path, sbin_table[i].start, len, sbin);
                installed++;
            }
        }
        pr_info("initramfs: installed %zu /sbin tools\n", installed);
    }

    /* OpenRC configuration tree (empty when the image carries no blob). */
    {
        size_t len = (size_t)(_binary_openrc_rootfs_orcf_end -
                              _binary_openrc_rootfs_orcf_start);
        if (len)
            install_rc_tree(_binary_openrc_rootfs_orcf_start, len);
    }

    /* `.new` archives staged for live exercises:
     *   newpkg verify /tmp/hello-new-1.0.0-x86_64.new
     *   newpkg install /tmp/hello-new-1.0.0-x86_64.new
     *   newpkg install /tmp/gnu-coreutils.new
     *   newpkg install /tmp/newos-sdk.new   (C SDK for /bin/tcc)
     * The first is built deterministically by tools/newpkg/newpkg-build.py,
     * the second by scripts/pack-gnu.sh (`make gnu && make gnu-pkg`), the
     * third by scripts/pack-sdk.sh from the generated sysroot. */
    {
        struct {
            const char *path;
            const u8 *start;
            const u8 *end;
        } pkgs[] = {
            { "/tmp/hello-new-1.0.0-x86_64.new",
              _binary_sample_new_start, _binary_sample_new_end },
            { "/tmp/gnu-coreutils.new",
              _binary_gnu_coreutils_new_start,
              _binary_gnu_coreutils_new_end },
            { "/tmp/newos-sdk.new",
              _binary_newos_sdk_new_start,
              _binary_newos_sdk_new_end },
        };
        for (size_t i = 0; i < sizeof(pkgs) / sizeof(pkgs[0]); i++) {
            size_t len = (size_t)(pkgs[i].end - pkgs[i].start);
            struct vfs_file *pf;
            if (!len)
                continue;
            if (vfs_create(pkgs[i].path) != 0) {
                pr_warn("initramfs: cannot stage %s\n", pkgs[i].path);
                continue;
            }
            pf = vfs_open(pkgs[i].path, VFS_O_READ | VFS_O_WRITE);
            if (!pf) {
                pr_warn("initramfs: cannot open %s\n", pkgs[i].path);
                continue;
            }
            if (vfs_write(pf, pkgs[i].start, len) != (int)len)
                pr_warn("initramfs: short write %s\n", pkgs[i].path);
            vfs_close(pf);
            pr_info("initramfs: staged %s (%zu bytes)\n", pkgs[i].path, len);
        }
    }

    pr_info("initramfs: built-in root populated\n");
}
