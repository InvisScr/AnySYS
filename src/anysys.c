/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * AnySYS - an experimental "(Almost) Universal SYS Command".
 *
 * This is original code guided by public behavior/ABI documentation:
 *   FreeDOS SYS help: https://help.fdos.org/en/hhstndrd/base/sys.htm
 *   INT 25h/26h docs: https://stanislavs.org/helppc/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#if defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__)
#include <dos.h>
#endif

#define ANYSYS_VERSION "0.2"
#define SECTOR_SIZE 512
#define MAX_PATH_LEN 128
#define MAX_NAME_LEN 13

#define ATTR_READONLY 0x01
#define ATTR_HIDDEN   0x02
#define ATTR_SYSTEM   0x04
#define ATTR_ARCHIVE  0x20
#define ATTR_VOLUME   0x08
#define ATTR_DIR      0x10
#define ATTR_LFN      0x0F

#define FAT_FREE      0x0000UL
#define FAT12_EOC     0x0FFFUL
#define FAT16_EOC     0xFFFFUL
#define DIR_ENTRY_SIZE 32

typedef unsigned char byte;
typedef unsigned short word;
typedef unsigned long dword;

typedef struct {
    dword start_sector;
    word sector_count;
    word buffer_offset;
    word buffer_segment;
} AbsDiskPacket;

typedef struct {
    word bytes_per_sector;
    byte sectors_per_cluster;
    word reserved_sectors;
    byte fats;
    word root_entries;
    dword total_sectors;
    byte media;
    word sectors_per_fat;
    dword fat_start;
    dword root_start;
    dword root_sectors;
    dword data_start;
    dword cluster_count;
    int fat_bits;
} FatInfo;

typedef struct {
    char src[MAX_PATH_LEN];
    char dst_name[MAX_NAME_LEN];
    byte short_name[11];
    dword size;
    dword clusters_needed;
    dword start_cluster;
} KernelPlan;

typedef enum {
    OEM_AUTO = 0,
    OEM_FD,
    OEM_MS,
    OEM_W9X,
    OEM_PC,
    OEM_DR,
    OEM_EDR,
    OEM_TOSHIBA
} OemKind;

typedef struct {
    OemKind kind;
    const char *name;
    const char *kernel1;
    const char *kernel2;
    const char *shell;
    word load_segment;
} OemProfile;

typedef struct {
    char source[MAX_PATH_LEN];
    char target_drive;
    char boot_image[MAX_PATH_LEN];
    char boot_source[MAX_PATH_LEN];
    char backup_bs[MAX_PATH_LEN];
    char dump_bs[MAX_PATH_LEN];
    char restore_bs[MAX_PATH_LEN];
    char kernel_name[MAX_NAME_LEN];
    char src_kernel_name[MAX_PATH_LEN];
    char src_shell_name[MAX_PATH_LEN];
    OemKind requested_oem;
    int have_source;
    int have_target;
    int bootonly;
    int update;
    int dryrun;
    int verbose;
    int help;
} Options;

static const OemProfile profiles[] = {
    { OEM_FD,  "FD",  "KERNEL.SYS",  NULL,        "COMMAND.COM", 0x0060 },
    { OEM_MS,  "MS",  "IO.SYS",      "MSDOS.SYS", "COMMAND.COM", 0x0070 },
    { OEM_W9X, "W9X", "IO.SYS",      "MSDOS.SYS", "COMMAND.COM", 0x0070 },
    { OEM_PC,  "PC",  "IBMBIO.COM",  "IBMDOS.COM","COMMAND.COM", 0x0070 },
    { OEM_DR,  "DR",  "IBMBIO.COM",  "IBMDOS.COM","COMMAND.COM", 0x0070 },
    { OEM_EDR, "EDR", "IBMBIO.COM",  "IBMDOS.COM","COMMAND.COM", 0x0070 },
    /* Toshiba's MS-DOS 2.11 OEM build (T1100 Plus) renames the kernel
       pair. Its boot sector expects TBIOS.SYS and TDOS.SYS in the first
       two root entries, the same layout raw placement gives IO.SYS. */
    { OEM_TOSHIBA, "TOSHIBA", "TBIOS.SYS", "TDOS.SYS", "COMMAND.COM", 0x0070 }
};

static byte copy_buffer[8192];
static byte boot_current[SECTOR_SIZE];
static byte boot_source[SECTOR_SIZE];
static byte boot_merged[SECTOR_SIZE];
static byte disk_sector[SECTOR_SIZE];
static byte disk_sector2[SECTOR_SIZE];

#if defined(__WATCOMC__) && (defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__))
static AbsDiskPacket abs_packet;
#endif

static int raw_install_system_kernels(const Options *opt, const OemProfile *profile);

static void init_options(Options *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->requested_oem = OEM_AUTO;
    strcpy(opt->kernel_name, "KERNEL.SYS");
}

static int streqi(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static int starts_with_i(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) {
            return 0;
        }
        ++s;
        ++prefix;
    }
    return 1;
}

static void str_to_upper(char *s)
{
    while (*s != '\0') {
        *s = (char)toupper((unsigned char)*s);
        ++s;
    }
}

static int copy_string(char *dst, size_t dstsz, const char *src)
{
    size_t len = strlen(src);
    if (len >= dstsz) {
        return 0;
    }
    memcpy(dst, src, len + 1);
    return 1;
}

static int append_string(char *dst, size_t dstsz, const char *src)
{
    size_t have = strlen(dst);
    size_t add = strlen(src);
    if (have + add >= dstsz) {
        return 0;
    }
    memcpy(dst + have, src, add + 1);
    return 1;
}

static int is_option(const char *arg)
{
    return arg[0] == '/' || arg[0] == '-';
}

static int is_drive_spec(const char *arg)
{
    return isalpha((unsigned char)arg[0]) && arg[1] == ':' && arg[2] == '\0';
}

static const char *option_value(const char *arg)
{
    const char *p = strchr(arg, ':');
    if (p == NULL) {
        p = strchr(arg, '=');
    }
    if (p == NULL) {
        return NULL;
    }
    return p + 1;
}

static const OemProfile *profile_by_kind(OemKind kind)
{
    unsigned i;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        if (profiles[i].kind == kind) {
            return &profiles[i];
        }
    }
    return &profiles[0];
}

static int parse_oem(const char *value, OemKind *kind)
{
    if (value == NULL || *value == '\0' || streqi(value, "AUTO")) {
        *kind = OEM_AUTO;
    } else if (streqi(value, "FD")) {
        *kind = OEM_FD;
    } else if (streqi(value, "MS")) {
        *kind = OEM_MS;
    } else if (streqi(value, "W9X") || streqi(value, "WIN9X")) {
        *kind = OEM_W9X;
    } else if (streqi(value, "PC")) {
        *kind = OEM_PC;
    } else if (streqi(value, "DR")) {
        *kind = OEM_DR;
    } else if (streqi(value, "EDR")) {
        *kind = OEM_EDR;
    } else if (streqi(value, "TOSHIBA") || streqi(value, "TOSH")) {
        *kind = OEM_TOSHIBA;
    } else {
        return 0;
    }
    return 1;
}

static int set_option_path(char *dst, size_t dstsz, const char *arg)
{
    const char *value = option_value(arg);
    if (value == NULL || *value == '\0') {
        return 0;
    }
    return copy_string(dst, dstsz, value);
}

static int parse_args(int argc, char **argv, Options *opt)
{
    int i;
    int positional_count = 0;
    const char *positionals[3];

    init_options(opt);

    if (argc <= 1) {
        opt->help = 1;
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (is_option(arg)) {
            const char *body = arg + 1;
            if (*body == '-') {
                ++body;
            }

            if (streqi(body, "?") || streqi(body, "HELP")) {
                opt->help = 1;
            } else if (streqi(body, "BOOTONLY")) {
                opt->bootonly = 1;
            } else if (streqi(body, "UPDATE")) {
                opt->update = 1;
            } else if (streqi(body, "DRYRUN") || streqi(body, "D")) {
                opt->dryrun = 1;
            } else if (streqi(body, "VERBOSE")) {
                opt->verbose = 1;
            } else if (streqi(body, "OEM") ||
                       starts_with_i(body, "OEM:") ||
                       starts_with_i(body, "OEM=")) {
                const char *value = option_value(body);
                if (value == NULL && body[3] == '\0') {
                    opt->requested_oem = OEM_AUTO;
                } else if (!parse_oem(value, &opt->requested_oem)) {
                    fprintf(stderr, "ANYSYS: unknown OEM profile '%s'\n",
                            value == NULL ? "" : value);
                    return 0;
                }
            } else if (starts_with_i(body, "BOOTSRC:") || starts_with_i(body, "BOOTSRC=")) {
                if (!set_option_path(opt->boot_source, sizeof(opt->boot_source), body)) {
                    fprintf(stderr, "ANYSYS: /BOOTSRC requires a file name\n");
                    return 0;
                }
            } else if (starts_with_i(body, "BACKUPBS:") || starts_with_i(body, "BACKUPBS=")) {
                if (!set_option_path(opt->backup_bs, sizeof(opt->backup_bs), body)) {
                    fprintf(stderr, "ANYSYS: /BACKUPBS requires a file name\n");
                    return 0;
                }
            } else if (starts_with_i(body, "DUMPBS:") || starts_with_i(body, "DUMPBS=")) {
                if (!set_option_path(opt->dump_bs, sizeof(opt->dump_bs), body)) {
                    fprintf(stderr, "ANYSYS: /DUMPBS requires a file name\n");
                    return 0;
                }
            } else if (starts_with_i(body, "RESTORBS:") || starts_with_i(body, "RESTORBS=")) {
                if (!set_option_path(opt->restore_bs, sizeof(opt->restore_bs), body)) {
                    fprintf(stderr, "ANYSYS: /RESTORBS requires a file name\n");
                    return 0;
                }
            } else if (starts_with_i(body, "K:") || starts_with_i(body, "K=")) {
                if (!set_option_path(opt->kernel_name, sizeof(opt->kernel_name), body)) {
                    fprintf(stderr, "ANYSYS: /K requires an 8.3 kernel name\n");
                    return 0;
                }
                str_to_upper(opt->kernel_name);
            } else if (starts_with_i(body, "SKFN:") || starts_with_i(body, "SKFN=")) {
                if (!set_option_path(opt->src_kernel_name, sizeof(opt->src_kernel_name), body)) {
                    fprintf(stderr, "ANYSYS: /SKFN requires a file name\n");
                    return 0;
                }
            } else if (starts_with_i(body, "SCFN:") || starts_with_i(body, "SCFN=")) {
                if (!set_option_path(opt->src_shell_name, sizeof(opt->src_shell_name), body)) {
                    fprintf(stderr, "ANYSYS: /SCFN requires a file name\n");
                    return 0;
                }
            } else {
                fprintf(stderr, "ANYSYS: unknown option '%s'\n", arg);
                return 0;
            }
        } else {
            if (positional_count >= 3) {
                fprintf(stderr, "ANYSYS: too many positional arguments\n");
                return 0;
            }
            positionals[positional_count++] = arg;
        }
    }

    if (positional_count == 1) {
        if (!is_drive_spec(positionals[0])) {
            fprintf(stderr, "ANYSYS: target drive is required\n");
            return 0;
        }
        opt->target_drive = (char)toupper((unsigned char)positionals[0][0]);
        opt->have_target = 1;
    } else if (positional_count == 2) {
        if (is_drive_spec(positionals[0]) && is_drive_spec(positionals[1])) {
            if (!copy_string(opt->source, sizeof(opt->source), positionals[0])) {
                fprintf(stderr, "ANYSYS: source path is too long\n");
                return 0;
            }
            opt->have_source = 1;
            opt->target_drive = (char)toupper((unsigned char)positionals[1][0]);
            opt->have_target = 1;
        } else if (!is_drive_spec(positionals[0]) && is_drive_spec(positionals[1])) {
            if (!copy_string(opt->source, sizeof(opt->source), positionals[0])) {
                fprintf(stderr, "ANYSYS: source path is too long\n");
                return 0;
            }
            opt->have_source = 1;
            opt->target_drive = (char)toupper((unsigned char)positionals[1][0]);
            opt->have_target = 1;
        } else if (is_drive_spec(positionals[0])) {
            opt->target_drive = (char)toupper((unsigned char)positionals[0][0]);
            opt->have_target = 1;
            if (!copy_string(opt->boot_image, sizeof(opt->boot_image), positionals[1])) {
                fprintf(stderr, "ANYSYS: boot sector image path is too long\n");
                return 0;
            }
        } else {
            fprintf(stderr, "ANYSYS: target drive is required\n");
            return 0;
        }
    } else if (positional_count == 3) {
        if (!is_drive_spec(positionals[1])) {
            fprintf(stderr, "ANYSYS: middle positional argument must be target drive\n");
            return 0;
        }
        if (!copy_string(opt->source, sizeof(opt->source), positionals[0])) {
            fprintf(stderr, "ANYSYS: source path is too long\n");
            return 0;
        }
        opt->have_source = 1;
        opt->target_drive = (char)toupper((unsigned char)positionals[1][0]);
        opt->have_target = 1;
        if (!copy_string(opt->boot_image, sizeof(opt->boot_image), positionals[2])) {
            fprintf(stderr, "ANYSYS: boot sector image path is too long\n");
            return 0;
        }
    }

    if (!opt->have_source) {
        strcpy(opt->source, ".");
    }

    return 1;
}

static void print_usage(void)
{
    puts("AnySYS " ANYSYS_VERSION " - (Almost) Universal SYS Command");
    puts("");
    puts("Usage:");
    puts("  ANYSYS [source] drive: [bootsect.img] [options]");
    puts("  ANYSYS drive: /DUMPBS:file");
    puts("  ANYSYS drive: /RESTORBS:file");
    puts("");
    puts("Options:");
    puts("  /OEM[:AUTO|FD|MS|W9X|PC|DR|EDR|TOSHIBA]");
    puts("                                   Select DOS family.");
    puts("  /BOOTSRC:file                    512-byte boot sector template.");
    puts("  /BOOTONLY                        Update boot sector only.");
    puts("  /UPDATE                          Copy kernel files, not COMMAND.COM.");
    puts("  /K:name                          Destination kernel name for FD-style OEM.");
    puts("  /SKFN:name                       Source kernel file override.");
    puts("  /SCFN:name                       Source COMMAND.COM override.");
    puts("  /BACKUPBS:file                   Save current boot sector first.");
    puts("  /DUMPBS:file                     Save current boot sector and exit.");
    puts("  /RESTORBS:file                   Restore exact 512-byte boot sector image.");
    puts("  /DRYRUN                          Show actions without writing.");
    puts("  /VERBOSE                         More detail.");
}

static int get_dos_attrs(const char *path, unsigned *attr)
{
#if defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__)
    return _dos_getfileattr(path, attr) == 0;
#else
    (void)path;
    (void)attr;
    return 0;
#endif
}

static void attrs_to_text(unsigned attr, char *out)
{
    out[0] = (attr & ATTR_READONLY) ? 'R' : '-';
    out[1] = (attr & ATTR_HIDDEN) ? 'H' : '-';
    out[2] = (attr & ATTR_SYSTEM) ? 'S' : '-';
    out[3] = (attr & ATTR_ARCHIVE) ? 'A' : '-';
    out[4] = '\0';
}

static void show_attrs(const char *label, const char *path)
{
    unsigned attr;
    char text[5];

    if (get_dos_attrs(path, &attr)) {
        attrs_to_text(attr, text);
        printf("%s attrs %s %s\n", label, text, path);
    }
}

static int file_exists(const char *path)
{
    FILE *f;
    unsigned attr;

    if (get_dos_attrs(path, &attr)) {
        return 1;
    }

    f = fopen(path, "rb");
    if (f != NULL) {
        fclose(f);
        return 1;
    }
    return 0;
}

static int read_file_exact(const char *path, byte *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    int extra;

    if (f == NULL) {
        fprintf(stderr, "ANYSYS: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }

    n = fread(buf, 1, size, f);
    extra = fgetc(f);
    fclose(f);

    if (n != size || extra != EOF) {
        fprintf(stderr, "ANYSYS: %s is not exactly %lu bytes\n",
                path, (unsigned long)size);
        return 0;
    }
    return 1;
}

static int write_file_exact(const char *path, const byte *buf, size_t size, int dryrun)
{
    FILE *f;

    if (dryrun) {
        printf("Would write %lu bytes to %s\n", (unsigned long)size, path);
        return 1;
    }

    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "ANYSYS: cannot create %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fwrite(buf, 1, size, f) != size) {
        fprintf(stderr, "ANYSYS: write failed for %s\n", path);
        fclose(f);
        return 0;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "ANYSYS: close failed for %s\n", path);
        return 0;
    }
    return 1;
}

static int ensure_sep(char *path, size_t pathsz)
{
    size_t len = strlen(path);
    if (len == 0) {
        return 1;
    }
    if (path[len - 1] == '\\' || path[len - 1] == '/' || path[len - 1] == ':') {
        return 1;
    }
    if (len + 1 >= pathsz) {
        return 0;
    }
    path[len] = '\\';
    path[len + 1] = '\0';
    return 1;
}

static int build_source_path(char *out, size_t outsz, const Options *opt,
                             const char *profile_name, const char *override_name)
{
    if (override_name != NULL && override_name[0] != '\0') {
        return copy_string(out, outsz, override_name);
    }

    if (!copy_string(out, outsz, opt->source)) {
        return 0;
    }
    if (!ensure_sep(out, outsz)) {
        return 0;
    }
    return append_string(out, outsz, profile_name);
}

static int build_dest_path(char *out, size_t outsz, char drive, const char *name)
{
    out[0] = drive;
    out[1] = ':';
    out[2] = '\\';
    out[3] = '\0';
    return append_string(out, outsz, name);
}

static int copy_file_stream(const char *src, const char *dst, int dryrun, int verbose)
{
    FILE *in;
    FILE *out;
    size_t n;

    if (dryrun) {
        printf("Would copy %s -> %s\n", src, dst);
        return 1;
    }

    if (verbose) {
        printf("Copying %s -> %s\n", src, dst);
        show_attrs("Source", src);
        show_attrs("Dest before", dst);
    }

#if defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__)
    {
        unsigned attr;
        if (_dos_getfileattr(dst, &attr) == 0) {
            attr &= ~(ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM);
            (void)_dos_setfileattr(dst, attr);
        }
    }
#endif

    in = fopen(src, "rb");
    if (in == NULL) {
        unsigned attr;
        fprintf(stderr, "ANYSYS: cannot open %s: %s\n", src, strerror(errno));
        if (get_dos_attrs(src, &attr)) {
            char text[5];
            attrs_to_text(attr, text);
            fprintf(stderr, "ANYSYS: source exists with attributes %s\n", text);
        }
        return 0;
    }

    out = fopen(dst, "wb");
    if (out == NULL) {
        fprintf(stderr, "ANYSYS: cannot create %s: %s\n", dst, strerror(errno));
        fclose(in);
        return 0;
    }

    while ((n = fread(copy_buffer, 1, sizeof(copy_buffer), in)) > 0) {
        if (fwrite(copy_buffer, 1, n, out) != n) {
            fprintf(stderr, "ANYSYS: write failed for %s\n", dst);
            fclose(out);
            fclose(in);
            return 0;
        }
    }

    if (ferror(in)) {
        fprintf(stderr, "ANYSYS: read failed for %s\n", src);
        fclose(out);
        fclose(in);
        return 0;
    }

    if (fclose(out) != 0) {
        fprintf(stderr, "ANYSYS: close failed for %s\n", dst);
        fclose(in);
        return 0;
    }
    fclose(in);

    if (verbose) {
        show_attrs("Dest after copy", dst);
    }
    return 1;
}

static void set_system_attrs(const char *path, int dryrun, int verbose)
{
#if defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__)
    unsigned attr;
    if (dryrun) {
        printf("Would mark %s as hidden/system/read-only\n", path);
        return;
    }
    if (_dos_getfileattr(path, &attr) == 0) {
        attr |= ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE;
        if (_dos_setfileattr(path, attr) != 0 && verbose) {
            printf("Warning: could not set attributes on %s\n", path);
        }
    }
#else
    (void)path;
    (void)dryrun;
    (void)verbose;
#endif
}

static int profile_files_exist(const Options *opt, const OemProfile *profile)
{
    char path[MAX_PATH_LEN];

    if (!build_source_path(path, sizeof(path), opt, profile->kernel1, NULL)) {
        return 0;
    }
    if (!file_exists(path)) {
        return 0;
    }

    if (profile->kernel2 != NULL) {
        if (!build_source_path(path, sizeof(path), opt, profile->kernel2, NULL)) {
            return 0;
        }
        if (!file_exists(path)) {
            return 0;
        }
    }

    if (!build_source_path(path, sizeof(path), opt, profile->shell, NULL)) {
        return 0;
    }
    return file_exists(path);
}

static const OemProfile *choose_profile(const Options *opt)
{
    unsigned i;
    if (opt->requested_oem != OEM_AUTO) {
        return profile_by_kind(opt->requested_oem);
    }

    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        if (profile_files_exist(opt, &profiles[i])) {
            return &profiles[i];
        }
    }

    return profile_by_kind(OEM_FD);
}

static const char *effective_fd_kernel_name(const Options *opt, const OemProfile *profile)
{
    if (profile->kind == OEM_FD && opt->kernel_name[0] != '\0') {
        return opt->kernel_name;
    }
    return profile->kernel1;
}

static int copy_one_system_file(const Options *opt, const char *src_name,
                                const char *dst_name, const char *override_name,
                                int mark_system)
{
    char src[MAX_PATH_LEN];
    char dst[MAX_PATH_LEN];

    if (!build_source_path(src, sizeof(src), opt, src_name, override_name) ||
        !build_dest_path(dst, sizeof(dst), opt->target_drive, dst_name)) {
        fprintf(stderr, "ANYSYS: path is too long while copying %s\n", dst_name);
        return 0;
    }

    if (!copy_file_stream(src, dst, opt->dryrun, opt->verbose)) {
        return 0;
    }
    if (mark_system) {
        set_system_attrs(dst, opt->dryrun, opt->verbose);
    }
    return 1;
}

static int copy_system_files(const Options *opt, const OemProfile *profile)
{
    const char *dst_kernel1 = effective_fd_kernel_name(opt, profile);

    if (opt->bootonly) {
        if (opt->verbose) {
            puts("Skipping system-file copy because /BOOTONLY was used.");
        }
        return 1;
    }

    printf("Installing %s system files to %c:\\\n", profile->name, opt->target_drive);

    if (profile->kernel2 != NULL && profile->kind != OEM_FD) {
        if (!raw_install_system_kernels(opt, profile)) {
            return 0;
        }
    } else {
        if (!copy_one_system_file(opt, profile->kernel1, dst_kernel1,
                                  opt->src_kernel_name, 1)) {
            return 0;
        }
    }

    if (!opt->update) {
        if (!copy_one_system_file(opt, profile->shell, profile->shell,
                                  opt->src_shell_name, 0)) {
            return 0;
        }
    } else if (opt->verbose) {
        puts("Skipping COMMAND.COM because /UPDATE was used.");
    }

    return 1;
}

static word get_le_word(const byte *p)
{
    return (word)(p[0] | ((word)p[1] << 8));
}

static dword get_le_dword(const byte *p)
{
    return (dword)p[0] |
           ((dword)p[1] << 8) |
           ((dword)p[2] << 16) |
           ((dword)p[3] << 24);
}

static void put_le_word(byte *p, word value)
{
    p[0] = (byte)(value & 0xFF);
    p[1] = (byte)((value >> 8) & 0xFF);
}

static void put_le_dword(byte *p, dword value)
{
    p[0] = (byte)(value & 0xFF);
    p[1] = (byte)((value >> 8) & 0xFF);
    p[2] = (byte)((value >> 16) & 0xFF);
    p[3] = (byte)((value >> 24) & 0xFF);
}

static int dos_read_logical_sector(char drive, dword logical_sector, byte *sector);
static int dos_write_logical_sector(char drive, dword logical_sector, const byte *sector);
static int raw_install_system_kernels(const Options *opt, const OemProfile *profile);
static void dos_flush_disk_buffers(void);
static void dos_login_drive(char drive);

static int looks_like_boot_sector(const byte *sector)
{
    word bytes_per_sector = get_le_word(sector + 11);
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0;
    }
    if (bytes_per_sector != 512) {
        return 0;
    }
    return 1;
}

static unsigned boot_code_offset_for_bpb(const byte *sector)
{
    word root_entries = get_le_word(sector + 17);
    if (root_entries == 0) {
        return 90; /* FAT32 EBPB commonly ends before offset 5Ah. */
    }
    return 62;     /* FAT12/16 EBPB commonly ends before offset 3Eh. */
}

/* Where the template's own code begins, per the jump it opens with, or 0
   if it has no recognisable jump. Pre-DOS 4 boot sectors start their code
   well before 3Eh - Toshiba's MS-DOS 2.11 jumps to 24h - so a BPB copy
   that runs to 3Eh regardless lands on the loader itself. */
static unsigned template_entry(const byte *sector)
{
    if (sector[0] == 0xEB) {
        return (unsigned)sector[1] + 2;
    }
    if (sector[0] == 0xE9) {
        return (unsigned)get_le_word(sector + 1) + 3;
    }
    return 0;
}

static int merge_boot_template(const byte *target_sector, const byte *template_sector,
                               byte *out_sector)
{
    unsigned offset;
    unsigned entry;

    if (!looks_like_boot_sector(target_sector)) {
        fprintf(stderr, "ANYSYS: target sector does not look like a FAT boot sector\n");
        return 0;
    }
    if (!looks_like_boot_sector(template_sector)) {
        fprintf(stderr, "ANYSYS: boot template does not look like a FAT boot sector\n");
        return 0;
    }

    offset = boot_code_offset_for_bpb(target_sector);

    /* Only the BPB the template has room for is carried over: everything
       from its jump target on is code. A jump into the BPB itself (below
       the 2.x fields' end at 1Eh) is not a layout we understand. */
    entry = template_entry(template_sector);
    if (entry != 0 && entry < offset) {
        if (entry < 0x1E) {
            fprintf(stderr, "ANYSYS: boot template jumps into its own BPB (%02Xh)\n",
                    entry);
            return 0;
        }
        offset = entry;
    }

    memcpy(out_sector, template_sector, SECTOR_SIZE);
    memcpy(out_sector + 3, target_sector + 3, offset - 3);
    out_sector[510] = 0x55;
    out_sector[511] = 0xAA;
    return 1;
}

#if defined(__WATCOMC__) && (defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__))
static unsigned dos_major_version(void)
{
    unsigned major = 0;

    _asm {
        mov ah, 30h
        int 21h
        xor ah, ah
        mov major, ax
    }
    return major;
}

static void dos_flush_disk_buffers(void)
{
    _asm {
        mov ah, 0dh
        int 21h
    }
}

/* Make DOS look at the disk in the drive before any absolute I/O.
   INT 25h/26h map logical sectors through whatever geometry DOS holds
   for the drive, and until something has made DOS read the disk that
   can be the drive's default rather than the media's own. Under MS-DOS
   6.22 on QEMU, a 720K floppy nobody had touched yet was addressed as
   single sided: sectors 0-8 came back right and everything from 9 on
   came from the wrong track. The boot sector, FAT and first root sector
   all live below 9, so it went unnoticed until something read file data.
   Get Free Space goes through the media check, which rebuilds the
   geometry from the disk's own BPB. Its result is not needed; an
   unreadable disk is reported by the first real read instead. */
static void dos_login_drive(char drive)
{
    unsigned drive_num = (unsigned)(toupper((unsigned char)drive) - 'A' + 1);

    _asm {
        mov ah, 36h
        mov dx, drive_num
        int 21h
    }
}

static void prepare_abs_packet(dword logical_sector, const byte *sector)
{
    unsigned data_segment = 0;

    _asm {
        mov ax, ds
        mov data_segment, ax
    }

    abs_packet.start_sector = logical_sector;
    abs_packet.sector_count = 1;
    abs_packet.buffer_offset = (word)(unsigned)sector;
    abs_packet.buffer_segment = (word)data_segment;
}
#endif

static int dos_read_logical_sector(char drive, dword logical_sector, byte *sector)
{
#if defined(__WATCOMC__) && (defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__))
    unsigned rc = 0;
    unsigned drive_num = (unsigned)(toupper((unsigned char)drive) - 'A');
    unsigned dos_major = dos_major_version();
    word sector_low = (word)(logical_sector & 0xFFFFUL);

    if (dos_major < 4 && logical_sector > 0xFFFFUL) {
        return -1;
    }

    prepare_abs_packet(logical_sector, sector);

    if (dos_major >= 4) {
        _asm {
            push ds
            mov ax, drive_num
            xor ah, ah
            mov cx, 0ffffh
            lea bx, abs_packet
            int 25h
            pop bx
            jnc read25_ok4
            mov rc, ax
read25_ok4:
            pop ds
        }
    } else {
        _asm {
            push ds
            mov ax, drive_num
            xor ah, ah
            mov cx, 1
            mov dx, sector_low
            mov bx, sector
            int 25h
            pop bx
            jnc read25_ok
            mov rc, ax
read25_ok:
            pop ds
        }
    }
    return (int)rc;
#else
    (void)drive;
    (void)logical_sector;
    (void)sector;
    return -1;
#endif
}

static int dos_write_logical_sector(char drive, dword logical_sector, const byte *sector)
{
#if defined(__WATCOMC__) && (defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__))
    unsigned rc = 0;
    unsigned drive_num = (unsigned)(toupper((unsigned char)drive) - 'A');
    unsigned dos_major = dos_major_version();
    word sector_low = (word)(logical_sector & 0xFFFFUL);

    if (dos_major < 4 && logical_sector > 0xFFFFUL) {
        return -1;
    }

    prepare_abs_packet(logical_sector, sector);

    if (dos_major >= 4) {
        _asm {
            push ds
            mov ax, drive_num
            xor ah, ah
            mov cx, 0ffffh
            lea bx, abs_packet
            int 26h
            pop bx
            jnc write26_ok4
            mov rc, ax
write26_ok4:
            pop ds
        }
    } else {
        _asm {
            push ds
            mov ax, drive_num
            xor ah, ah
            mov cx, 1
            mov dx, sector_low
            mov bx, sector
            int 26h
            pop bx
            jnc write26_ok
            mov rc, ax
write26_ok:
            pop ds
        }
    }
    return (int)rc;
#else
    (void)drive;
    (void)logical_sector;
    (void)sector;
    return -1;
#endif
}

#if !(defined(__WATCOMC__) && (defined(__DOS__) || defined(MSDOS) || defined(__MSDOS__)))
static void dos_flush_disk_buffers(void)
{
}

static void dos_login_drive(char drive)
{
    (void)drive;
}
#endif

static int dos_read_boot_sector(char drive, byte *sector)
{
    return dos_read_logical_sector(drive, 0, sector);
}

static int dos_write_boot_sector(char drive, const byte *sector)
{
    return dos_write_logical_sector(drive, 0, sector);
}

static int parse_fat_info(const byte *boot, FatInfo *fat)
{
    dword data_sectors;

    memset(fat, 0, sizeof(*fat));

    if (!looks_like_boot_sector(boot)) {
        fprintf(stderr, "ANYSYS: target does not have a recognizable FAT boot sector\n");
        return 0;
    }

    fat->bytes_per_sector = get_le_word(boot + 11);
    fat->sectors_per_cluster = boot[13];
    fat->reserved_sectors = get_le_word(boot + 14);
    fat->fats = boot[16];
    fat->root_entries = get_le_word(boot + 17);
    fat->total_sectors = get_le_word(boot + 19);
    if (fat->total_sectors == 0) {
        fat->total_sectors = get_le_dword(boot + 32);
    }
    fat->media = boot[21];
    fat->sectors_per_fat = get_le_word(boot + 22);

    if (fat->bytes_per_sector != SECTOR_SIZE ||
        fat->sectors_per_cluster == 0 ||
        fat->reserved_sectors == 0 ||
        fat->fats == 0 ||
        fat->root_entries == 0 ||
        fat->sectors_per_fat == 0 ||
        fat->total_sectors == 0) {
        fprintf(stderr, "ANYSYS: raw placement currently supports FAT12/16 only\n");
        return 0;
    }

    fat->root_sectors =
        (((dword)fat->root_entries * DIR_ENTRY_SIZE) + (SECTOR_SIZE - 1)) /
        SECTOR_SIZE;
    fat->fat_start = fat->reserved_sectors;
    fat->root_start = fat->fat_start + ((dword)fat->fats * fat->sectors_per_fat);
    fat->data_start = fat->root_start + fat->root_sectors;

    if (fat->data_start >= fat->total_sectors) {
        fprintf(stderr, "ANYSYS: FAT geometry is inconsistent\n");
        return 0;
    }

    data_sectors = fat->total_sectors - fat->data_start;
    fat->cluster_count = data_sectors / fat->sectors_per_cluster;

    if (fat->cluster_count < 4085UL) {
        fat->fat_bits = 12;
    } else if (fat->cluster_count < 65525UL) {
        fat->fat_bits = 16;
    } else {
        fprintf(stderr, "ANYSYS: raw placement does not support FAT32 yet\n");
        return 0;
    }

    return 1;
}

static dword cluster_to_sector(const FatInfo *fat, dword cluster)
{
    return fat->data_start + ((cluster - 2) * fat->sectors_per_cluster);
}

static int read_root_entry(char drive, const FatInfo *fat, word index, byte *entry)
{
    dword sector = fat->root_start +
                   (((dword)index * DIR_ENTRY_SIZE) / SECTOR_SIZE);
    unsigned offset = (unsigned)(((dword)index * DIR_ENTRY_SIZE) % SECTOR_SIZE);

    if (index >= fat->root_entries) {
        return 0;
    }
    if (dos_read_logical_sector(drive, sector, disk_sector) != 0) {
        fprintf(stderr, "ANYSYS: cannot read root directory sector %lu\n",
                (unsigned long)sector);
        return 0;
    }
    memcpy(entry, disk_sector + offset, DIR_ENTRY_SIZE);
    return 1;
}

static int write_root_entry(char drive, const FatInfo *fat, word index,
                            const byte *entry, int dryrun)
{
    dword sector = fat->root_start +
                   (((dword)index * DIR_ENTRY_SIZE) / SECTOR_SIZE);
    unsigned offset = (unsigned)(((dword)index * DIR_ENTRY_SIZE) % SECTOR_SIZE);

    if (index >= fat->root_entries) {
        return 0;
    }
    if (dryrun) {
        printf("Would write root directory entry %u\n", index);
        return 1;
    }
    if (dos_read_logical_sector(drive, sector, disk_sector) != 0) {
        fprintf(stderr, "ANYSYS: cannot read root directory sector %lu\n",
                (unsigned long)sector);
        return 0;
    }
    memcpy(disk_sector + offset, entry, DIR_ENTRY_SIZE);
    if (dos_write_logical_sector(drive, sector, disk_sector) != 0) {
        fprintf(stderr, "ANYSYS: cannot write root directory sector %lu\n",
                (unsigned long)sector);
        return 0;
    }
    return 1;
}

static int root_entry_free(const byte *entry)
{
    return entry[0] == 0x00 || entry[0] == 0xE5;
}

static int root_entry_matches(const byte *entry, const byte short_name[11])
{
    if (root_entry_free(entry) || entry[11] == ATTR_LFN) {
        return 0;
    }
    return memcmp(entry, short_name, 11) == 0;
}

static int find_free_root_slot(char drive, const FatInfo *fat, word first_index,
                               word *slot)
{
    byte entry[DIR_ENTRY_SIZE];
    word i;

    for (i = first_index; i < fat->root_entries; ++i) {
        if (!read_root_entry(drive, fat, i, entry)) {
            return 0;
        }
        if (root_entry_free(entry)) {
            *slot = i;
            return 1;
        }
    }
    return 0;
}

static int parse_short_name(const char *name, byte out[11])
{
    unsigned i = 0;
    unsigned j = 0;

    memset(out, ' ', 11);

    if (name[0] == '\0' || name[0] == '.') {
        return 0;
    }

    while (name[i] != '\0' && name[i] != '.') {
        if (j >= 8 || name[i] == ' ' || name[i] == '\\' || name[i] == '/') {
            return 0;
        }
        out[j++] = (byte)toupper((unsigned char)name[i++]);
    }

    if (name[i] == '.') {
        ++i;
        j = 8;
        while (name[i] != '\0') {
            if (j >= 11 || name[i] == ' ' || name[i] == '.' ||
                name[i] == '\\' || name[i] == '/') {
                return 0;
            }
            out[j++] = (byte)toupper((unsigned char)name[i++]);
        }
    }

    return 1;
}

static int get_file_size(const char *path, dword *size)
{
    FILE *f = fopen(path, "rb");
    long end_pos;

    if (f == NULL) {
        fprintf(stderr, "ANYSYS: cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    if (fseek(f, 0L, SEEK_END) != 0) {
        fprintf(stderr, "ANYSYS: cannot seek %s\n", path);
        fclose(f);
        return 0;
    }
    end_pos = ftell(f);
    fclose(f);

    if (end_pos <= 0) {
        fprintf(stderr, "ANYSYS: %s has invalid size\n", path);
        return 0;
    }
    *size = (dword)end_pos;
    return 1;
}

static dword fat_eoc_value(const FatInfo *fat)
{
    return fat->fat_bits == 12 ? FAT12_EOC : FAT16_EOC;
}

static int fat_value_is_eoc(const FatInfo *fat, dword value)
{
    if (fat->fat_bits == 12) {
        return value >= 0x0FF8UL;
    }
    return value >= 0xFFF8UL;
}

/* FAT reads go through a two-sector cache. Relocating clusters scans the
   whole FAT once per cluster moved, and without the cache every entry
   would cost an INT 25h. Two slots, chosen by sector parity, because a
   FAT12 entry can straddle two consecutive sectors. Any FAT write drops
   the cache. */
#define FAT_CACHE_NONE 0xFFFFFFFFUL
static byte fat_cache[2][SECTOR_SIZE];
static dword fat_cache_lsn[2] = { FAT_CACHE_NONE, FAT_CACHE_NONE };

static void fat_cache_drop(void)
{
    fat_cache_lsn[0] = FAT_CACHE_NONE;
    fat_cache_lsn[1] = FAT_CACHE_NONE;
}

static const byte *fat_sector(char drive, dword lsn)
{
    unsigned slot = (unsigned)(lsn & 1UL);

    if (fat_cache_lsn[slot] != lsn) {
        if (dos_read_logical_sector(drive, lsn, fat_cache[slot]) != 0) {
            fat_cache_lsn[slot] = FAT_CACHE_NONE;
            return NULL;
        }
        fat_cache_lsn[slot] = lsn;
    }
    return fat_cache[slot];
}

static int get_fat_entry(char drive, const FatInfo *fat, dword cluster, dword *value)
{
    dword offset;
    dword sector;
    unsigned pos;
    const byte *a;
    const byte *b;

    if (cluster < 2 || cluster > fat->cluster_count + 1) {
        return 0;
    }

    if (fat->fat_bits == 12) {
        offset = cluster + (cluster / 2);
        sector = fat->fat_start + (offset / SECTOR_SIZE);
        pos = (unsigned)(offset % SECTOR_SIZE);

        if ((a = fat_sector(drive, sector)) == NULL) {
            return 0;
        }
        if (pos == SECTOR_SIZE - 1) {
            if ((b = fat_sector(drive, sector + 1)) == NULL) {
                return 0;
            }
            if ((cluster & 1) == 0) {
                *value = a[pos] | ((dword)(b[0] & 0x0F) << 8);
            } else {
                *value = ((dword)a[pos] >> 4) | ((dword)b[0] << 4);
            }
        } else {
            if ((cluster & 1) == 0) {
                *value = a[pos] | ((dword)(a[pos + 1] & 0x0F) << 8);
            } else {
                *value = ((dword)a[pos] >> 4) | ((dword)a[pos + 1] << 4);
            }
        }
        *value &= 0x0FFFUL;
    } else {
        offset = cluster * 2;
        sector = fat->fat_start + (offset / SECTOR_SIZE);
        pos = (unsigned)(offset % SECTOR_SIZE);
        if ((a = fat_sector(drive, sector)) == NULL) {
            return 0;
        }
        *value = get_le_word(a + pos);
    }
    return 1;
}

static int set_fat_entry_one(char drive, const FatInfo *fat, byte fat_index,
                             dword cluster, dword value, int dryrun)
{
    dword fat_base = fat->fat_start + ((dword)fat_index * fat->sectors_per_fat);
    dword offset;
    dword sector;
    unsigned pos;

    if (dryrun) {
        return 1;
    }
    fat_cache_drop();

    if (fat->fat_bits == 12) {
        offset = cluster + (cluster / 2);
        sector = fat_base + (offset / SECTOR_SIZE);
        pos = (unsigned)(offset % SECTOR_SIZE);

        if (dos_read_logical_sector(drive, sector, disk_sector) != 0) {
            return 0;
        }
        if (pos == SECTOR_SIZE - 1) {
            if (dos_read_logical_sector(drive, sector + 1, disk_sector2) != 0) {
                return 0;
            }
            if ((cluster & 1) == 0) {
                disk_sector[pos] = (byte)(value & 0xFF);
                disk_sector2[0] = (byte)((disk_sector2[0] & 0xF0) |
                                         ((value >> 8) & 0x0F));
            } else {
                disk_sector[pos] = (byte)((disk_sector[pos] & 0x0F) |
                                          ((value << 4) & 0xF0));
                disk_sector2[0] = (byte)((value >> 4) & 0xFF);
            }
            if (dos_write_logical_sector(drive, sector, disk_sector) != 0 ||
                dos_write_logical_sector(drive, sector + 1, disk_sector2) != 0) {
                return 0;
            }
        } else {
            if ((cluster & 1) == 0) {
                disk_sector[pos] = (byte)(value & 0xFF);
                disk_sector[pos + 1] = (byte)((disk_sector[pos + 1] & 0xF0) |
                                              ((value >> 8) & 0x0F));
            } else {
                disk_sector[pos] = (byte)((disk_sector[pos] & 0x0F) |
                                          ((value << 4) & 0xF0));
                disk_sector[pos + 1] = (byte)((value >> 4) & 0xFF);
            }
            if (dos_write_logical_sector(drive, sector, disk_sector) != 0) {
                return 0;
            }
        }
    } else {
        offset = cluster * 2;
        sector = fat_base + (offset / SECTOR_SIZE);
        pos = (unsigned)(offset % SECTOR_SIZE);
        if (dos_read_logical_sector(drive, sector, disk_sector) != 0) {
            return 0;
        }
        put_le_word(disk_sector + pos, (word)value);
        if (dos_write_logical_sector(drive, sector, disk_sector) != 0) {
            return 0;
        }
    }

    return 1;
}

static int set_fat_entry_all(char drive, const FatInfo *fat, dword cluster,
                             dword value, int dryrun)
{
    byte i;

    if (dryrun) {
        printf("Would set FAT cluster %lu to %lu\n",
               (unsigned long)cluster, (unsigned long)value);
        return 1;
    }

    for (i = 0; i < fat->fats; ++i) {
        if (!set_fat_entry_one(drive, fat, i, cluster, value, dryrun)) {
            fprintf(stderr, "ANYSYS: cannot update FAT %u cluster %lu\n",
                    (unsigned)i, (unsigned long)cluster);
            return 0;
        }
    }
    return 1;
}

static int free_cluster_chain(char drive, const FatInfo *fat, dword start_cluster,
                              int dryrun)
{
    dword cluster = start_cluster;
    dword next;
    dword guard = 0;

    if (cluster < 2 || cluster > fat->cluster_count + 1) {
        return 1;
    }

    while (cluster >= 2 && cluster <= fat->cluster_count + 1 &&
           guard++ < fat->cluster_count) {
        if (!get_fat_entry(drive, fat, cluster, &next)) {
            return 0;
        }
        if (!set_fat_entry_all(drive, fat, cluster, FAT_FREE, dryrun)) {
            return 0;
        }
        if (next == FAT_FREE || fat_value_is_eoc(fat, next)) {
            break;
        }
        cluster = next;
    }
    return 1;
}

static int delete_existing_root_entries(char drive, const FatInfo *fat,
                                        const KernelPlan *plans,
                                        unsigned plan_count, int dryrun)
{
    byte entry[DIR_ENTRY_SIZE];
    byte deleted[DIR_ENTRY_SIZE];
    word i;
    unsigned p;

    memset(deleted, 0, sizeof(deleted));
    deleted[0] = 0xE5;

    for (i = 0; i < fat->root_entries; ++i) {
        if (!read_root_entry(drive, fat, i, entry)) {
            return 0;
        }
        if (entry[0] == 0x00) {
            break;
        }
        if (entry[0] == 0xE5 || entry[11] == ATTR_LFN) {
            continue;
        }
        for (p = 0; p < plan_count; ++p) {
            if (root_entry_matches(entry, plans[p].short_name)) {
                dword start_cluster = get_le_word(entry + 26);
                if (!free_cluster_chain(drive, fat, start_cluster, dryrun)) {
                    return 0;
                }
                if (!write_root_entry(drive, fat, i, deleted, dryrun)) {
                    return 0;
                }
                break;
            }
        }
    }
    return 1;
}

static int make_reserved_root_slots(char drive, const FatInfo *fat,
                                    unsigned reserved_count, int dryrun)
{
    byte entry[DIR_ENTRY_SIZE];
    byte deleted[DIR_ENTRY_SIZE];
    word dst_slot;
    unsigned i;

    memset(deleted, 0, sizeof(deleted));
    deleted[0] = 0xE5;

    for (i = 0; i < reserved_count; ++i) {
        if (!read_root_entry(drive, fat, (word)i, entry)) {
            return 0;
        }
        if (root_entry_free(entry)) {
            continue;
        }
        if (entry[11] == ATTR_LFN) {
            fprintf(stderr,
                    "ANYSYS: cannot move a long-name entry from root slot %u\n", i);
            return 0;
        }
        if (!find_free_root_slot(drive, fat, (word)reserved_count, &dst_slot)) {
            fprintf(stderr, "ANYSYS: no free root directory slot to move entry %u\n", i);
            return 0;
        }
        if (dryrun) {
            printf("Would move root entry %u to %u\n", i, dst_slot);
        }
        if (!write_root_entry(drive, fat, dst_slot, entry, dryrun) ||
            !write_root_entry(drive, fat, (word)i, deleted, dryrun)) {
            return 0;
        }
    }
    return 1;
}

static int cluster_in_plans(dword cluster, const KernelPlan *plans,
                            unsigned plan_count)
{
    unsigned i;

    for (i = 0; i < plan_count; ++i) {
        if (plans[i].start_cluster != 0 &&
            cluster >= plans[i].start_cluster &&
            cluster < plans[i].start_cluster + plans[i].clusters_needed) {
            return 1;
        }
    }
    return 0;
}

static int find_contiguous_free_run_avoiding(char drive, const FatInfo *fat,
                                             dword clusters_needed,
                                             const KernelPlan *plans,
                                             unsigned plan_count,
                                             dword *start_cluster)
{
    dword cluster;
    dword run_start = 0;
    dword run_len = 0;
    dword value;

    for (cluster = 2; cluster <= fat->cluster_count + 1; ++cluster) {
        if (!get_fat_entry(drive, fat, cluster, &value)) {
            return 0;
        }
        if (value == FAT_FREE && !cluster_in_plans(cluster, plans, plan_count)) {
            if (run_len == 0) {
                run_start = cluster;
            }
            ++run_len;
            if (run_len >= clusters_needed) {
                *start_cluster = run_start;
                return 1;
            }
        } else {
            run_len = 0;
        }
    }

    return 0;
}

static int write_file_to_contiguous_clusters(char drive, const FatInfo *fat,
                                             const KernelPlan *plan, int dryrun)
{
    FILE *f;
    dword cluster_index;
    byte sector_index;
    size_t n;

    if (dryrun) {
        printf("Would write %s to clusters %lu-%lu\n",
               plan->src,
               (unsigned long)plan->start_cluster,
               (unsigned long)(plan->start_cluster + plan->clusters_needed - 1));
        return 1;
    }

    f = fopen(plan->src, "rb");
    if (f == NULL) {
        fprintf(stderr, "ANYSYS: cannot open %s: %s\n", plan->src, strerror(errno));
        return 0;
    }

    for (cluster_index = 0; cluster_index < plan->clusters_needed; ++cluster_index) {
        dword cluster = plan->start_cluster + cluster_index;
        dword first_sector = cluster_to_sector(fat, cluster);

        for (sector_index = 0; sector_index < fat->sectors_per_cluster; ++sector_index) {
            memset(disk_sector, 0, sizeof(disk_sector));
            n = fread(disk_sector, 1, SECTOR_SIZE, f);
            if (ferror(f)) {
                fprintf(stderr, "ANYSYS: read failed for %s\n", plan->src);
                fclose(f);
                return 0;
            }
            if (dos_write_logical_sector(drive, first_sector + sector_index,
                                         disk_sector) != 0) {
                fprintf(stderr, "ANYSYS: cannot write data sector %lu\n",
                        (unsigned long)(first_sector + sector_index));
                fclose(f);
                return 0;
            }
            if (n < SECTOR_SIZE && feof(f)) {
                /* The remaining clusters, if any, are already zero-filled. */
            }
        }
    }

    fclose(f);
    return 1;
}

static int allocate_contiguous_chain(char drive, const FatInfo *fat,
                                     const KernelPlan *plan, int dryrun)
{
    dword i;

    for (i = 0; i < plan->clusters_needed; ++i) {
        dword cluster = plan->start_cluster + i;
        dword next = (i + 1 < plan->clusters_needed) ?
                     (cluster + 1) : fat_eoc_value(fat);
        if (!set_fat_entry_all(drive, fat, cluster, next, dryrun)) {
            return 0;
        }
    }
    return 1;
}

static void make_dos_datetime(word *dos_date, word *dos_time)
{
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    unsigned year;

    if (tm_now == NULL || tm_now->tm_year + 1900 < 1980) {
        *dos_date = (word)((1 << 5) | 1);
        *dos_time = 0;
        return;
    }

    year = (unsigned)(tm_now->tm_year + 1900 - 1980);
    if (year > 127) {
        year = 127;
    }
    *dos_date = (word)((year << 9) |
                       ((tm_now->tm_mon + 1) << 5) |
                       tm_now->tm_mday);
    *dos_time = (word)((tm_now->tm_hour << 11) |
                       (tm_now->tm_min << 5) |
                       (tm_now->tm_sec / 2));
}

static void make_kernel_dir_entry(byte *entry, const KernelPlan *plan)
{
    word dos_date;
    word dos_time;

    memset(entry, 0, DIR_ENTRY_SIZE);
    memcpy(entry, plan->short_name, 11);
    entry[11] = ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_ARCHIVE;
    make_dos_datetime(&dos_date, &dos_time);
    put_le_word(entry + 22, dos_time);
    put_le_word(entry + 24, dos_date);
    put_le_word(entry + 26, (word)plan->start_cluster);
    put_le_dword(entry + 28, plan->size);
}

static int prepare_kernel_plan(const Options *opt, const FatInfo *fat,
                               const char *src_name, const char *dst_name,
                               const char *override_name, KernelPlan *plan)
{
    dword cluster_bytes = (dword)fat->sectors_per_cluster * SECTOR_SIZE;

    memset(plan, 0, sizeof(*plan));

    if (!build_source_path(plan->src, sizeof(plan->src), opt, src_name,
                           override_name) ||
        !copy_string(plan->dst_name, sizeof(plan->dst_name), dst_name)) {
        fprintf(stderr, "ANYSYS: path is too long while planning %s\n", dst_name);
        return 0;
    }
    if (!parse_short_name(plan->dst_name, plan->short_name)) {
        fprintf(stderr, "ANYSYS: %s is not a valid 8.3 system file name\n",
                plan->dst_name);
        return 0;
    }
    if (!get_file_size(plan->src, &plan->size)) {
        return 0;
    }

    plan->clusters_needed = (plan->size + cluster_bytes - 1) / cluster_bytes;
    if (plan->clusters_needed == 0 ||
        plan->clusters_needed > fat->cluster_count) {
        fprintf(stderr, "ANYSYS: %s is too large for this FAT volume\n", plan->src);
        return 0;
    }
    return 1;
}

/* ---- clearing the start of the data area -----------------------------
 *
 * DOS 2.x and 3.x loaders do not follow the FAT to find the first kernel:
 * they read it from the first sectors of the data area, which is cluster
 * 2. Later loaders take the start cluster from the root entry, but
 * cluster 2 satisfies those too, so the first kernel always goes there.
 *
 * On a disk that already holds files, whatever occupies those clusters is
 * moved out of the way first, one cluster at a time: copy the data, point
 * whatever referred to the old cluster - the previous link in its chain,
 * or the directory entry that starts the file - at the new one, then free
 * the old one. Done in that order, an interruption leaves at worst a lost
 * cluster for CHKDSK, never a file pointing at freed space.
 */

#define MAX_DIR_STACK 256

typedef struct {
    dword cluster;      /* directory cluster being read; 0 in the root */
    dword lsn;          /* sector currently in dir_buf                 */
    unsigned left;      /* sectors still to read in this root/cluster  */
    unsigned slot;      /* next entry in buf, 0..15                    */
    int ended;
} DirIter;

static byte dir_buf[SECTOR_SIZE];
static dword dir_stack[MAX_DIR_STACK];

static int fat_value_is_bad(const FatInfo *fat, dword value)
{
    if (fat->fat_bits == 12) {
        return value >= 0x0FF0UL && value <= 0x0FF7UL;
    }
    return value >= 0xFFF0UL && value <= 0xFFF7UL;
}

static int fat_value_is_link(const FatInfo *fat, dword value)
{
    return value >= 2 && value <= fat->cluster_count + 1;
}

static void dir_open(DirIter *it, const FatInfo *fat, dword start)
{
    it->cluster = start;
    it->slot = SECTOR_SIZE / DIR_ENTRY_SIZE;
    it->ended = 0;
    /* lsn sits one before the first sector; dir_next pre-increments. */
    if (start == 0) {
        it->lsn = fat->root_start - 1;
        it->left = (unsigned)fat->root_sectors;
    } else {
        it->lsn = cluster_to_sector(fat, start) - 1;
        it->left = fat->sectors_per_cluster;
    }
}

/* The next entry of the directory, in dir_buf at *offset; the sector it
   came from stays in dir_buf until the next call, so a caller can edit
   the entry and write it->lsn back. 1 = entry, 0 = end, -1 = error. */
static int dir_next(char drive, const FatInfo *fat, DirIter *it, unsigned *offset)
{
    dword next;

    if (it->ended) {
        return 0;
    }
    if (it->slot >= SECTOR_SIZE / DIR_ENTRY_SIZE) {
        if (it->left == 0) {
            if (it->cluster == 0) {
                it->ended = 1;
                return 0;
            }
            if (!get_fat_entry(drive, fat, it->cluster, &next)) {
                return -1;
            }
            if (!fat_value_is_link(fat, next)) {
                it->ended = 1;
                return 0;
            }
            it->cluster = next;
            it->lsn = cluster_to_sector(fat, next) - 1;
            it->left = fat->sectors_per_cluster;
        }
        ++it->lsn;
        if (dos_read_logical_sector(drive, it->lsn, dir_buf) != 0) {
            fprintf(stderr, "ANYSYS: cannot read directory sector %lu\n",
                    (unsigned long)it->lsn);
            return -1;
        }
        --it->left;
        it->slot = 0;
    }

    *offset = it->slot * DIR_ENTRY_SIZE;
    ++it->slot;
    if (dir_buf[*offset] == 0x00) {
        it->ended = 1;
        return 0;
    }
    return 1;
}

static int dir_write_back(char drive, const DirIter *it)
{
    if (dos_write_logical_sector(drive, it->lsn, dir_buf) != 0) {
        fprintf(stderr, "ANYSYS: cannot write directory sector %lu\n",
                (unsigned long)it->lsn);
        return 0;
    }
    return 1;
}

/* An entry that names a file or directory with clusters of its own. */
static int dir_entry_owns_clusters(const byte *entry)
{
    return entry[0] != 0xE5 && entry[0] != '.' &&
           entry[11] != ATTR_LFN && !(entry[11] & ATTR_VOLUME);
}

/* The cluster whose FAT entry points at target, if any. */
static int find_fat_predecessor(char drive, const FatInfo *fat, dword target,
                                dword *pred)
{
    dword c;
    dword value;

    for (c = 2; c <= fat->cluster_count + 1; ++c) {
        if (!get_fat_entry(drive, fat, c, &value)) {
            return -1;
        }
        if (value == target) {
            *pred = c;
            return 1;
        }
    }
    return 0;
}

/* Find the directory entry whose file or directory starts at from, and
   point it at to. The whole tree is searched, depth first, with pending
   directories on an explicit stack. *is_dir reports what was moved. */
static int repoint_chain_head(char drive, const FatInfo *fat, dword from,
                              dword to, int *is_dir)
{
    DirIter it;
    unsigned n = 1;
    unsigned off;
    int rc;
    byte *e;
    dword start;

    dir_stack[0] = 0;
    while (n > 0) {
        dir_open(&it, fat, dir_stack[--n]);
        while ((rc = dir_next(drive, fat, &it, &off)) > 0) {
            e = dir_buf + off;
            if (!dir_entry_owns_clusters(e)) {
                continue;
            }
            start = get_le_word(e + 26);
            if (start == from) {
                put_le_word(e + 26, (word)to);
                *is_dir = (e[11] & ATTR_DIR) != 0;
                return dir_write_back(drive, &it) ? 1 : -1;
            }
            if ((e[11] & ATTR_DIR) && start >= 2) {
                if (n >= MAX_DIR_STACK) {
                    fprintf(stderr, "ANYSYS: directory tree too large to search\n");
                    return -1;
                }
                dir_stack[n++] = start;
            }
        }
        if (rc < 0) {
            return -1;
        }
    }
    return 0;
}

/* A directory's first cluster moved from -> to: its own "." entry and the
   ".." entry of each directory inside it still name the old cluster. */
static int fix_moved_directory(char drive, const FatInfo *fat, dword from, dword to)
{
    DirIter it;
    unsigned off;
    int rc;
    byte *e;
    dword child;

    dir_open(&it, fat, to);
    while ((rc = dir_next(drive, fat, &it, &off)) > 0) {
        e = dir_buf + off;
        if (memcmp(e, ".          ", 11) == 0) {
            if (get_le_word(e + 26) == from) {
                put_le_word(e + 26, (word)to);
                if (!dir_write_back(drive, &it)) {
                    return 0;
                }
            }
            continue;
        }
        if (!dir_entry_owns_clusters(e) || !(e[11] & ATTR_DIR)) {
            continue;
        }
        child = get_le_word(e + 26);
        if (child < 2) {
            continue;
        }
        /* ".." is the second entry of the child's first sector. The
           child is read into disk_sector, leaving dir_buf - and so this
           walk - undisturbed. */
        if (dos_read_logical_sector(drive, cluster_to_sector(fat, child),
                                    disk_sector) != 0) {
            return 0;
        }
        if (memcmp(disk_sector + DIR_ENTRY_SIZE, "..         ", 11) == 0 &&
            get_le_word(disk_sector + DIR_ENTRY_SIZE + 26) == from) {
            put_le_word(disk_sector + DIR_ENTRY_SIZE + 26, (word)to);
            if (dos_write_logical_sector(drive, cluster_to_sector(fat, child),
                                         disk_sector) != 0) {
                return 0;
            }
        }
    }
    return rc == 0;
}

static int move_cluster(char drive, const FatInfo *fat, dword from, dword to)
{
    dword next;
    dword pred;
    byte s;
    int rc;
    int is_dir = 0;

    for (s = 0; s < fat->sectors_per_cluster; ++s) {
        if (dos_read_logical_sector(drive, cluster_to_sector(fat, from) + s,
                                    disk_sector) != 0 ||
            dos_write_logical_sector(drive, cluster_to_sector(fat, to) + s,
                                     disk_sector) != 0) {
            fprintf(stderr, "ANYSYS: cannot copy cluster %lu to %lu\n",
                    (unsigned long)from, (unsigned long)to);
            return 0;
        }
    }

    if (!get_fat_entry(drive, fat, from, &next) ||
        !set_fat_entry_all(drive, fat, to, next, 0)) {
        return 0;
    }

    rc = find_fat_predecessor(drive, fat, from, &pred);
    if (rc < 0) {
        return 0;
    }
    if (rc > 0) {
        if (!set_fat_entry_all(drive, fat, pred, to, 0)) {
            return 0;
        }
    } else {
        rc = repoint_chain_head(drive, fat, from, to, &is_dir);
        if (rc < 0) {
            return 0;
        }
        if (rc == 0) {
            fprintf(stderr,
                    "ANYSYS: cluster %lu is in use but no file owns it;\n"
                    "        run CHKDSK /F on the target first\n",
                    (unsigned long)from);
            /* Undo the copy's allocation; nothing points at it yet. */
            set_fat_entry_all(drive, fat, to, FAT_FREE, 0);
            return 0;
        }
        if (is_dir && !fix_moved_directory(drive, fat, from, to)) {
            return 0;
        }
    }

    return set_fat_entry_all(drive, fat, from, FAT_FREE, 0);
}

/* Empty clusters first .. first+count-1 by moving whatever is there to
   free clusters after them. */
static int clear_cluster_run(const Options *opt, const FatInfo *fat,
                             dword first, dword count, const char *for_name)
{
    char drive = opt->target_drive;
    dword c;
    dword value;
    dword to = first + count;
    dword moved = 0;

    if (first + count - 1 > fat->cluster_count + 1) {
        fprintf(stderr, "ANYSYS: %s does not fit at cluster %lu\n",
                for_name, (unsigned long)first);
        return 0;
    }

    for (c = first; c < first + count; ++c) {
        if (!get_fat_entry(drive, fat, c, &value)) {
            return 0;
        }
        if (value == FAT_FREE) {
            continue;
        }
        if (fat_value_is_bad(fat, value)) {
            fprintf(stderr,
                    "ANYSYS: cluster %lu is marked bad; %s cannot start at "
                    "cluster %lu\n",
                    (unsigned long)c, for_name, (unsigned long)first);
            return 0;
        }

        if (opt->dryrun) {
            ++moved;
            continue;
        }

        /* The next free cluster beyond the run. Searching on from the last
           one used keeps the moved clusters in their original order. */
        for (;; ++to) {
            if (to > fat->cluster_count + 1) {
                fprintf(stderr,
                        "ANYSYS: no free cluster to move cluster %lu to\n",
                        (unsigned long)c);
                return 0;
            }
            if (!get_fat_entry(drive, fat, to, &value)) {
                return 0;
            }
            if (value == FAT_FREE) {
                break;
            }
        }

        if (!move_cluster(drive, fat, c, to)) {
            return 0;
        }
        ++moved;
        ++to;
    }

    if (moved > 0 && (opt->verbose || opt->dryrun)) {
        printf("%s %lu cluster%s out of clusters %lu-%lu for %s\n",
               opt->dryrun ? "Would move" : "Moved",
               (unsigned long)moved, moved == 1 ? "" : "s",
               (unsigned long)first, (unsigned long)(first + count - 1),
               for_name);
    }
    return 1;
}

static int source_is_target_root(const Options *opt)
{
    const char *p = opt->source;

    if (!(isalpha((unsigned char)p[0]) && p[1] == ':')) {
        return 0;
    }
    if (toupper((unsigned char)p[0]) != opt->target_drive) {
        return 0;
    }
    p += 2;
    if (*p == '\0') {
        return 1;
    }
    if ((p[0] == '\\' || p[0] == '/') && p[1] == '\0') {
        return 1;
    }
    return 0;
}

static int raw_install_system_kernels(const Options *opt, const OemProfile *profile)
{
    FatInfo fat;
    KernelPlan plans[2];
    unsigned plan_count = 0;
    unsigned i;
    byte entry[DIR_ENTRY_SIZE];

    if (source_is_target_root(opt)) {
        fprintf(stderr,
                "ANYSYS: source cannot be the target root during raw replacement\n");
        fprintf(stderr,
                "        Copy the source system files to another drive or directory first.\n");
        return 0;
    }

    if (dos_read_boot_sector(opt->target_drive, boot_current) != 0 ||
        !parse_fat_info(boot_current, &fat)) {
        return 0;
    }

    if (profile->kernel2 == NULL) {
        return 0;
    }

    if (!prepare_kernel_plan(opt, &fat, profile->kernel1, profile->kernel1,
                             opt->src_kernel_name, &plans[0]) ||
        !prepare_kernel_plan(opt, &fat, profile->kernel2, profile->kernel2,
                             NULL, &plans[1])) {
        return 0;
    }
    plan_count = 2;

    if (opt->verbose || opt->dryrun) {
        printf("FAT%u: %lu clusters, %u sectors/cluster, root entries %u\n",
               fat.fat_bits,
               (unsigned long)fat.cluster_count,
               (unsigned)fat.sectors_per_cluster,
               fat.root_entries);
    }

    if (!delete_existing_root_entries(opt->target_drive, &fat, plans, plan_count,
                                      opt->dryrun) ||
        !make_reserved_root_slots(opt->target_drive, &fat, plan_count,
                                  opt->dryrun)) {
        return 0;
    }

    /* The first kernel goes at cluster 2, whatever is there now; see
       clear_cluster_run. The second only has to be contiguous. */
    if (!clear_cluster_run(opt, &fat, 2, plans[0].clusters_needed,
                           plans[0].dst_name)) {
        return 0;
    }
    plans[0].start_cluster = 2;

    for (i = 0; i < plan_count; ++i) {
        if (i > 0 &&
            !find_contiguous_free_run_avoiding(opt->target_drive, &fat,
                                               plans[i].clusters_needed,
                                               plans, i,
                                               &plans[i].start_cluster)) {
            fprintf(stderr,
                    "ANYSYS: no contiguous free run for %s (%lu clusters)\n",
                    plans[i].dst_name,
                    (unsigned long)plans[i].clusters_needed);
            return 0;
        }
        if (opt->verbose || opt->dryrun) {
            printf("%s: %lu bytes, %lu contiguous clusters at %lu\n",
                   plans[i].dst_name,
                   (unsigned long)plans[i].size,
                   (unsigned long)plans[i].clusters_needed,
                   (unsigned long)plans[i].start_cluster);
        }
    }

    for (i = 0; i < plan_count; ++i) {
        if (!write_file_to_contiguous_clusters(opt->target_drive, &fat,
                                               &plans[i], opt->dryrun) ||
            !allocate_contiguous_chain(opt->target_drive, &fat, &plans[i],
                                       opt->dryrun)) {
            return 0;
        }
    }

    for (i = 0; i < plan_count; ++i) {
        make_kernel_dir_entry(entry, &plans[i]);
        if (!write_root_entry(opt->target_drive, &fat, (word)i, entry,
                              opt->dryrun)) {
            return 0;
        }
    }

    if (!opt->dryrun) {
        dos_flush_disk_buffers();
    }
    puts("System kernels placed in the first root entries with contiguous FAT chains.");
    return 1;
}

static int read_target_boot_sector(const Options *opt, byte *sector)
{
    int rc;

    if (opt->boot_image[0] != '\0' && file_exists(opt->boot_image)) {
        if (opt->verbose) {
            printf("Reading target BPB from image %s\n", opt->boot_image);
        }
        return read_file_exact(opt->boot_image, sector, SECTOR_SIZE);
    }

    rc = dos_read_boot_sector(opt->target_drive, sector);
    if (rc != 0) {
        fprintf(stderr,
                "ANYSYS: cannot read boot sector from %c: (DOS error %d)\n",
                opt->target_drive, rc);
        return 0;
    }
    return 1;
}

static int write_target_boot_sector(const Options *opt, const byte *sector)
{
    int rc;

    if (opt->boot_image[0] != '\0') {
        return write_file_exact(opt->boot_image, sector, SECTOR_SIZE, opt->dryrun);
    }

    if (opt->dryrun) {
        printf("Would write boot sector to %c:\\\n", opt->target_drive);
        return 1;
    }

    rc = dos_write_boot_sector(opt->target_drive, sector);
    if (rc != 0) {
        fprintf(stderr,
                "ANYSYS: cannot write boot sector to %c: (DOS error %d)\n",
                opt->target_drive, rc);
        return 0;
    }
    dos_flush_disk_buffers();
    return 1;
}

static int handle_boot_sector(const Options *opt)
{
    if (opt->dump_bs[0] != '\0') {
        if (!read_target_boot_sector(opt, boot_current)) {
            return 0;
        }
        return write_file_exact(opt->dump_bs, boot_current, SECTOR_SIZE, opt->dryrun);
    }

    if (opt->backup_bs[0] != '\0') {
        if (!read_target_boot_sector(opt, boot_current)) {
            return 0;
        }
        if (!write_file_exact(opt->backup_bs, boot_current, SECTOR_SIZE, opt->dryrun)) {
            return 0;
        }
    }

    if (opt->restore_bs[0] != '\0') {
        if (!read_file_exact(opt->restore_bs, boot_source, SECTOR_SIZE)) {
            return 0;
        }
        if (opt->verbose) {
            printf("Restoring exact boot sector from %s\n", opt->restore_bs);
        }
        return write_target_boot_sector(opt, boot_source);
    }

    if (opt->boot_source[0] == '\0') {
        if (opt->bootonly) {
            fprintf(stderr, "ANYSYS: /BOOTONLY requires /BOOTSRC or /RESTORBS\n");
            return 0;
        }
        if (opt->verbose) {
            puts("No /BOOTSRC supplied; boot sector was not changed.");
        }
        return 1;
    }

    if (!read_target_boot_sector(opt, boot_current)) {
        return 0;
    }
    if (!read_file_exact(opt->boot_source, boot_source, SECTOR_SIZE)) {
        return 0;
    }
    if (!merge_boot_template(boot_current, boot_source, boot_merged)) {
        return 0;
    }

    if (opt->verbose) {
        printf("Installing boot code from %s with target BPB preserved\n",
               opt->boot_source);
    }
    return write_target_boot_sector(opt, boot_merged);
}

int main(int argc, char **argv)
{
    Options opt;
    const OemProfile *profile;

    if (!parse_args(argc, argv, &opt)) {
        puts("Use ANYSYS /? for help.");
        return 1;
    }

    if (opt.help) {
        print_usage();
        return 0;
    }

    if (!opt.have_target) {
        fprintf(stderr, "ANYSYS: target drive is required\n");
        puts("Use ANYSYS /? for help.");
        return 1;
    }

    /* Before the first INT 25h/26h on the target; see dos_login_drive. */
    dos_login_drive(opt.target_drive);

    profile = choose_profile(&opt);

    if (opt.verbose || opt.dryrun) {
        printf("Target: %c:\\\n", opt.target_drive);
        printf("Source: %s\n", opt.source);
        printf("OEM: %s\n", profile->name);
        if (opt.boot_image[0] != '\0') {
            printf("Boot image output: %s\n", opt.boot_image);
        }
    }

    if (!copy_system_files(&opt, profile)) {
        return 1;
    }

    if (!handle_boot_sector(&opt)) {
        return 1;
    }

    if (opt.dryrun) {
        puts("Dry run complete; no changes were written.");
    } else {
        puts("System transfer complete.");
    }
    return 0;
}
