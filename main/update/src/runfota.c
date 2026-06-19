#include "update_common.h"

/* ============ module-local framework model (externs + structs) ============ */
/* Parsed contents of /tmp/boot_control_flag, format "su_<version>_<SP>"
   e.g. "su_v1_30".  Populated by runfota_read_boot_control_flag.
   In the OEM ABI this is a struct returned by value: a std::string at
   offset 0 followed by two int8 fields (partition @0x20, stage @0x21);
   modelled here with a fixed version buffer. */
typedef struct BootControlFlag {
    char        version[64];  /* token[1], must equal service version "v1" */
    signed char partition;    /* token[2][1] - '0'  (0 or 1)               */
    signed char stage;        /* token[2][0] - '0'  (update_stage)         */
} BootControlFlag;

/* OEM 0x11b000 -- split the parked FOTA blob via runFOTA.sh -d split */
int runfota_split_fota_file(void);
/* OEM 0x11d220 -- read & parse /tmp/boot_control_flag (su_state env dump) */
bool runfota_read_boot_control_flag(BootControlFlag *out);
/* OEM 0x11dc80 -- write the su_state env via fw_setenv */
int runfota_set_boot_command_flag(int stage, unsigned char partition);
/* OEM 0x11b750 -- extract PRODUCT.VERSION from the FOTA header */
int runfota_read_fota_version_from_header(char *out_version, size_t out_cap);
/* Slurp an entire file into buf (models ifstream + ostringstream). Returns
   true on success; buf is NUL-terminated and capped at cap-1 bytes. */
bool file_read_all(const char *path, char *buf, size_t cap);
/* Read one line from an open stdio FILE (models std::istream::getline);
   strips the trailing newline. Returns false at EOF/error. */
bool fgets_line(FILE *f, char *buf, size_t cap);
/* Bounded copy that always NUL-terminates (models std::string assignment). */
void str_copy(char *dst, size_t cap, const char *src);
/* In-place strip of leading/trailing ASCII whitespace
   (models the OEM trim helper FUN_00157ef0 @0x157ef0). */
void str_trim(char *s);
/* common_logf log levels / update_stage values used here come from
   update_common.h: LOG_ERR, LOG_WARN, LOG_INFO, STAGE_UNKNOWN. */
/* ========================================================================== */

/*
 * runfota.c -- VanMoof i.MX8 A/B boot-control + FOTA-header logic.
 *
 * Behaviour-oriented reconstruction of the VanMoof part of
 * devices/main/update/src/runfota.cpp.  The actual firmware install steps are
 * thin system() wrappers around shell scripts (runFOTA.sh, fw_setenv/printenv,
 * cp, reboot); the shell command strings are reproduced verbatim as the OEM
 * global std::string constants (initialised in the binary's static ctor
 * _INIT_4).  The STL string/stream/vector machinery is modelled with the
 * str_* / format helpers; the heavy framework (nlohmann::json, mosquitto,
 * IUpdateClient) is out of scope for this TU.
 *
 * All OEM addresses are from program "update" (AArch64, image base 0x100000).
 */

#include <string.h>
#include <stdlib.h>   /* system() */
#include <sys/stat.h> /* stat()   */

/* ------------------------------------------------------------------ */
/* OEM global std::string constants (initialised in _INIT_4 @0x10ce50) */
/* ------------------------------------------------------------------ */

/* bootcontrol service version the flag must match (DAT_0016fdc0 "v1",
 * exposed as the std::string DAT_001a1f38). */
static const char *const SU_SERVICE_VERSION   = "v1";

/* shell command that dumps the u-boot env var su_state (DAT_001a1ef8). */
static const char *const FW_PRINTENV_SU_STATE = "fw_printenv -n su_state";

/* " > " redirect operator (string literal @0x171410). */
static const char *const REDIRECT_TO          = " > ";

/* the boot control flag file the env dump is redirected to,
 * then re-read (DAT_001a1f18). */
static const char *const BOOT_CONTROL_FLAG    = "/tmp/boot_control_flag";

/* set-side: "fw_setenv su_state" (DAT_001a1f58) + " " (@0x177da0)
 * + "su_v1_" (@0x1715e0). */
static const char *const FW_SETENV_SU_STATE   = "fw_setenv su_state";
static const char *const SP                   = " ";
static const char *const SU_V1_PREFIX         = "su_v1_";

/* FOTA-header split / scan (DAT_001a1cd8 / DAT_001a1e18 / DAT_001a1e38). */
static const char *const RUNFOTA_SPLIT_CMD    = "runFOTA.sh -d split";
static const char *const FOTA_HEADER_PATH     = "/tmp/fota/VM-XS5_FOTA_header";
static const char *const PRODUCT_VERSION_KEY  = "PRODUCT.VERSION=";
static const char *const EQUALS               = "=";  /* find_first_of arg @0x171178 */

#define SRC "devices/main/update/src/runfota.cpp"

/* ------------------------------------------------------------------ */
/* runfota_split_fota_file  -- OEM 0x11b000                           */
/* Splits the parked FOTA blob via `runFOTA.sh -d split`.             */
/* Returns 0 on success, -1 on system() failure.                     */
/* ------------------------------------------------------------------ */
int runfota_split_fota_file(void)
{
    int rc;

    common_logf(SRC, 0xe6, LOG_INFO, "Split FOTA File with cmd: %s",
                RUNFOTA_SPLIT_CMD);

    rc = system(RUNFOTA_SPLIT_CMD);
    if (rc != 0) {
        common_logf(SRC, 0xe9, LOG_ERR, "Failed splitting fota file: %d", rc);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* runfota_read_boot_control_flag  -- OEM 0x11d220                    */
/*                                                                    */
/* 1. `fw_printenv -n su_state > /tmp/boot_control_flag`  (system())  */
/* 2. slurp /tmp/boot_control_flag into a string                     */
/* 3. reject if empty                                                */
/* 4. split on '_' -> {"su","v1","<stage><partition>"}               */
/*    - out->version   = token[1]                                    */
/*    - out->partition = token[2][1] - '0'                           */
/*    - out->stage     = token[2][0] - '0'                           */
/* 5. verify version == service version "v1"                         */
/*                                                                    */
/* The OEM throws std::runtime_error on every failure; here the      */
/* throws are modelled as returning false (out is left valid only on */
/* the success path).                                                */
/* ------------------------------------------------------------------ */
bool runfota_read_boot_control_flag(BootControlFlag *out)
{
    char cmd[256];
    char content[256];
    char tokens[8][64];
    size_t ntok;
    const char *third;
    int rc;

    /* cmd = "fw_printenv -n su_state" + " > " + "/tmp/boot_control_flag" */
    cmd[0] = '\0';
    strncat(cmd, FW_PRINTENV_SU_STATE, sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, REDIRECT_TO,          sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, BOOT_CONTROL_FLAG,    sizeof(cmd) - strlen(cmd) - 1);

    rc = system(cmd);
    if (rc != 0) {
        common_logf(SRC, 0x3a, LOG_ERR,
                    "Failed to get boot command flag: %d", rc);
        /* OEM: throw std::runtime_error("Unable to retrieve boot control flag.") */
        return false;
    }

    /* slurp the file the env dump was redirected to (ifstream + stringstream) */
    if (!file_read_all(BOOT_CONTROL_FLAG, content, sizeof(content))) {
        content[0] = '\0';
    }

    /* empty check -- OEM compares the slurped string against "" */
    if (content[0] == '\0') {
        common_logf(SRC, 0x43, LOG_ERR, "Boot control flag is empty: %d", 0);
        /* OEM: throw std::runtime_error("Unable to retrieve boot control flag.") */
        return false;
    }

    /* split on '_'  ->  e.g. "su_v1_30" -> {"su","v1","30"} */
    ntok = str_split(content, "_", tokens, 8);

    /* OEM range-checks: needs >= 2 vector elements, the 3rd present,
     * and the 3rd token at least 2 chars long. */
    if (ntok < 2) {
        return false; /* OEM: vector::_M_range_check throw */
    }

    /* out->version = token[1] */
    str_copy(out->version, sizeof(out->version), tokens[1]);

    if (ntok < 3) {
        return false; /* OEM: vector::_M_range_check throw */
    }

    third = tokens[2];
    if (strlen(third) < 2) {
        return false; /* OEM: basic_string::at out_of_range throw */
    }

    /* digits decoded by subtracting '0'; layout: [0]=stage, [1]=partition */
    out->partition = (signed char)(third[1] - '0');
    out->stage     = (signed char)(third[0] - '0');

    common_logf(SRC, 0x51, LOG_INFO,
                "Boot control version: %s, partition: %d, stage: %d",
                out->version, out->partition, out->stage);

    /* version must equal the service's bootcontrol version "v1" */
    if (strcmp(out->version, SU_SERVICE_VERSION) != 0) {
        common_logf(SRC, 0x57, LOG_ERR,
                    "Bootcontrol version mismatch, service version %s, "
                    "bootcontrol version: %s ",
                    out->version, SU_SERVICE_VERSION);
        /* OEM: throw std::runtime_error("Bootcontrol version mismatch.") */
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/* runfota_set_boot_command_flag  -- OEM 0x11dc80                     */
/*                                                                    */
/* args: stage (update_stage), partition (0/1)                       */
/*                                                                    */
/* - reject UNKNOWN stage (== -1)                                    */
/* - refresh/validate the current flag (read_boot_control_flag)      */
/* - require partition < 2                                           */
/* - build the env value: each field formatted with "%d":           */
/*       <stage><part==0><part==0><stage>                            */
/*   (the OEM literally formats partition as the boolean expr        */
/*    `partition == 0`, twice)                                       */
/* - command: "fw_setenv su_state" + " " + "su_v1_" + <value>        */
/* - system() it                                                     */
/*                                                                    */
/* Returns 0 on success, -1 on any error.                            */
/* ------------------------------------------------------------------ */
int runfota_set_boot_command_flag(int stage, unsigned char partition)
{
    BootControlFlag cur;
    char value[64];
    char cmd[256];
    int part0;
    int rc;

    if (stage == STAGE_UNKNOWN) {  /* -1 */
        common_logf(SRC, 0x77, LOG_ERR, "cannot set to UNKNOWN update_stage");
        return -1;
    }

    /* OEM reads the current boot control flag here (side-effect: validates the
     * bootcontrol version; on a bad flag the read throws). cur is otherwise
     * unused by the set path. */
    (void)runfota_read_boot_control_flag(&cur);

    if (partition >= 2) {
        common_logf(SRC, 0x8d, LOG_ERR, "Error in partition value :%d",
                    (int)partition);
        return -1;
    }

    /* The OEM concatenation, in order, of four "%d"-formatted fields:
     *   format("%d", stage)
     *   format("%d", partition == 0)
     *   format("%d", partition == 0)
     *   format("%d", stage)
     * yielding e.g. stage=3,partition=0 -> "3113". */
    part0 = (partition == 0) ? 1 : 0;
    snprintf(value, sizeof(value), "%d%d%d%d", stage, part0, part0, stage);

    /* cmd = "fw_setenv su_state" + " " + "su_v1_" + value */
    cmd[0] = '\0';
    strncat(cmd, FW_SETENV_SU_STATE, sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, SP,                 sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, SU_V1_PREFIX,       sizeof(cmd) - strlen(cmd) - 1);
    strncat(cmd, value,              sizeof(cmd) - strlen(cmd) - 1);

    rc = system(cmd);
    if (rc != 0) {
        common_logf(SRC, 0x85, LOG_ERR,
                    "Failed to set boot command flag: %d", rc);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* runfota_read_fota_version_from_header  -- OEM 0x11b750             */
/*                                                                    */
/* - split the FOTA blob first (runfota_split_fota_file)             */
/* - stat() /tmp/fota/VM-XS5_FOTA_header; bail if missing            */
/* - read it line by line (ifstream::getline)                        */
/* - find the first line containing "PRODUCT.VERSION="               */
/* - take the substring after the first '=' -> out version           */
/* - trim whitespace                                                 */
/*                                                                    */
/* Returns 0 on success, -1 on failure (split failed / no header /   */
/* marker not found before EOF). The extracted version is written    */
/* through *out_version.                                             */
/* ------------------------------------------------------------------ */
int runfota_read_fota_version_from_header(char *out_version, size_t out_cap)
{
    FILE *f;
    struct stat st;
    char line[256];

    if (runfota_split_fota_file() != 0) {
        common_logf(SRC, 0xaf, LOG_ERR,
                    "Unable to split FOTA file, cannot retrieve FOTA version");
        return -1;
    }

    if (stat(FOTA_HEADER_PATH, &st) != 0) {
        common_logf(SRC, 0xb5, LOG_ERR, "Header file does not exist");
        return -1;
    }

    f = fopen(FOTA_HEADER_PATH, "r");
    if (f == NULL) {
        /* OEM: filebuf::open failed -> ios::badbit; the getline loop then
         * runs against a failed stream and exits at EOF (eofbit set). */
        return -1;
    }

    while (fgets_line(f, line, sizeof(line))) {
        const char *eq;

        if (!str_contains(line, PRODUCT_VERSION_KEY)) {
            continue;
        }

        /* substring after the first '=' (find_first_of "=") */
        eq = strpbrk(line, EQUALS);
        if (eq != NULL) {
            str_copy(out_version, out_cap, eq + 1);
        } else {
            str_copy(out_version, out_cap, line);
        }

        /* OEM trims surrounding whitespace (FUN_00157ef0 -> ltrim/rtrim) */
        str_trim(out_version);

        fclose(f);
        return 0;
    }

    fclose(f);
    return -1;  /* marker not found before EOF */
}
