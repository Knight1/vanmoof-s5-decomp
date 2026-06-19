#include "update_common.h"

/* ============ module-local framework model (externs + structs) ============ */
/*
 * ManifestEntry — OEM layout is 0x60 (96) bytes (operator_new(0x60)).
 *   +0x00  type tag / vptr (&DAT_0019cd50)         [modelled away]
 *   +0x08  std::string image_path   (ptr/len/inline buf, 0x20 bytes)
 *   +0x28  std::string device_name  (ptr/len/inline buf, 0x20 bytes)
 *   +0x48  uint32 version (packed semver)
 *   +0x50  uint64 reserved (written 0)
 *   +0x58  bool   allowed_to_skip
 *   +0x59  bool   skip_rollback
 * The packed `version` word is:
 *   (variant & 0xff) << 13 | (patch & 0x1fff) | (minor & 0xff) << 16 | major << 24
 */
typedef struct ManifestEntry {
    char     image_path[128];   /* dir + "/" + field[0] */
    char     device_name[64];   /* field[1], '-' -> '_' */
    uint32_t version;           /* packed semver (see manifest_pack_version) */
    bool     allowed_to_skip;   /* field[8] == 1 */
    bool     skip_rollback;     /* field[9] != 0 */
} ManifestEntry;

/* Growable list of entries (models std::vector<ManifestEntry> in the
 * update context at +0x08 begin / +0x10 end / +0x18 capacity). */
typedef struct ManifestEntryList {
    ManifestEntry *items;
    size_t         count;
    size_t         capacity;
} ManifestEntryList;

/*
 * UpdateContext — the OEM update-context object passed to manifest_parse.
 * Only the fields touched by this TU are modelled:
 *   +0x08..+0x18  std::vector<ManifestEntry> entries
 *   +0x20/+0x28   std::string manifest_dir (ptr/len)
 *   +0x40/+0x48   std::string manifest_name (ptr/len)
 */
typedef struct UpdateContext {
    ManifestEntryList entries;       /* +0x08 */
    const char       *manifest_dir;  /* +0x20 */
    const char       *manifest_name; /* +0x40 */
} UpdateContext;

/* Maximum CSV columns we keep room for when splitting a manifest row. */
#define MANIFEST_MAX_FIELDS 16
/* strtol-based integer parse (models libstdc++ std::stoi + the OEM wrapper
 * FUN_00119b50). Aborts via the framework on invalid/out-of-range input;
 * `err` is the label string ("stoi") used in the diagnostic. */
int upd_stoi(const char *str, const char *err, int base);
/* Exact-match string compare (models std::string::compare == 0). */
bool str_eq(const char *a, const char *b);
/* Bounded string copy (models std::string assignment). */
void str_copy(char *dst, const char *src, size_t dstsz);
/* Bounded string append (models std::string::operator+= / _M_append). */
void str_append(char *dst, const char *src, size_t dstsz);
/* ifstream-style open of the manifest text file (OEM std::ifstream, mode 8). */
void *manifest_open(const char *path);
/* Read one line; returns false on EOF/fail (mirrors the OEM std::getline guard). */
bool manifest_getline(void *fp, char *buf, size_t bufsz);
/* Close the manifest file (OEM std::ifstream destructor / filebuf::close). */
void manifest_close(void *fp);
/* Append a parsed entry to the context's growable entry list (models the
 * std::vector<ManifestEntry> push_back at ctx+0x08..0x18). */
void manifest_entries_push(ManifestEntryList *list, const ManifestEntry *entry);
/* ========================================================================== */

/*
 * manifest.c — VanMoof OTA "update" service, manifest.txt parser.
 *
 * Reconstructed from the AArch64 "update" image (base 0x100000).
 * Source path embedded in the binary: devices/main/update/src/manifest.cpp
 *
 * manifest.txt is a CSV-ish table (',' separated). The first line is a header
 * and is skipped. Every subsequent non-empty line is one ManifestEntry.
 *
 * A row is split on ',' into >= 7 fields (the OEM rejects rows that produce
 * fewer than 7 fields). The columns the parser consumes are:
 *
 *   [0] image file name   -> image_path = <manifest dir> + "/" + field
 *   [1] device name       -> '-' replaced by '_'
 *   [4] semver major       (strtol base 10)
 *   [5] semver minor       (strtol base 10)
 *   [6] semver patch       (strtol base 10)
 *   [7] build/variant tag  ("dev"->1, "rc"->2, "main"->3, other-nonempty->0)
 *   [8] allowed_to_skip    (strtol base 10; flag = (val == 1))   [optional, >=9 fields]
 *   [9] skip_rollback      (strtol base 10; flag = (val != 0))   [optional, >=10 fields]
 *
 * The 32-bit packed `version` is laid out as
 *   (variant & 0xff) << 13 | (patch & 0x1fff) | (minor & 0xff) << 16 | major << 24
 * (see manifest_pack_version / OEM FUN_00159ce0).
 */

/* OEM &DAT_00170918 — error string handed to the strtol wrapper. */
static const char STOI_ERR[] = "stoi";

/* OEM 0x177b18 / 0x177b20 / 0x177b28 — recognised build/variant tags. */
#define VARIANT_DEV   "dev"
#define VARIANT_RC    "rc"
#define VARIANT_MAIN  "main"

/*
 * OEM 0x00119b50 — strtol() wrapper used for every numeric manifest field.
 *
 * Saves/clears errno, calls strtol(str, &end, base); throws (here: aborts via
 * the framework helpers) on "no digits consumed", ERANGE, or a value outside
 * the int32 range. On success it restores errno and returns the int value.
 * Models libstdc++'s std::stoi.
 */
static int manifest_stoi(const char *str)
{
    return upd_stoi(str, STOI_ERR, 10);
}

/*
 * OEM 0x00159900 — FUN_00159900: build/variant tag -> small int.
 * "dev" -> 1, "rc" -> 2, "main" -> 3, any other NON-EMPTY string -> 0,
 * empty string -> 3 (the OEM falls through to 3 and only overrides to 0 when
 * the string length is non-zero).
 */
static int manifest_variant_code(const char *variant)
{
    if (str_eq(variant, VARIANT_DEV))
        return 1;
    if (str_eq(variant, VARIANT_RC))
        return 2;
    if (str_eq(variant, VARIANT_MAIN))
        return 3;
    /* default 3, overridden to 0 only when the field is non-empty */
    if (variant[0] != '\0')
        return 0;
    return 3;
}

/*
 * OEM 0x00159ce0 — FUN_00159ce0: pack semver into the 32-bit version word.
 *   (variant & 0xff) << 13 | (patch & 0x1fff) | (minor & 0xff) << 16 | major << 24
 */
static uint32_t manifest_pack_version(int major, int minor, int patch, int variant)
{
    return ((uint32_t)(variant & 0xff) << 13)
         | ((uint32_t)patch & 0x1fff)
         | ((uint32_t)(minor & 0xff) << 16)
         | ((uint32_t)major << 24);
}

/*
 * OEM 0x00118920 — manifest_parse_row.
 *
 * Splits one CSV line into fields, then fills *out. On a row with fewer than 7
 * fields (or where the first split yields begin==end) the OEM logs an error and
 * returns a stub entry with empty strings, version 0, both flags false.
 * Returns true when a real entry was produced, false for the stub/error path.
 *
 * `manifest_dir` is the parent directory of manifest.txt (the OEM reads it from
 * the update-context object at +0x20/+0x28); image_path = dir + "/" + field0.
 */
bool manifest_parse_row(ManifestEntry *out, const char *manifest_dir, const char *row)
{
    char fields[MANIFEST_MAX_FIELDS][64];
    size_t nfields;
    size_t i;

    nfields = str_split(row, ",", fields, MANIFEST_MAX_FIELDS);

    /* OEM: reject when the split produced no fields, or fewer than 7
     * (size in bytes < 0xe0 == 7 * sizeof(std::string)). */
    if (nfields == 0 || nfields < 7) {
        common_logf("devices/main/update/src/manifest.cpp", 0x38, LOG_ERR,
                    "Unable to parse manifest row: %s", row);
        out->image_path[0] = '\0';
        out->device_name[0] = '\0';
        out->version = 0;
        out->allowed_to_skip = false;
        out->skip_rollback = false;
        return false;
    }

    /* image_path = manifest_dir + "/" + field[0] */
    str_copy(out->image_path, manifest_dir, sizeof(out->image_path));
    str_append(out->image_path, "/", sizeof(out->image_path));
    str_append(out->image_path, fields[0], sizeof(out->image_path));

    /* device name: '-' -> '_' */
    str_copy(out->device_name, fields[1], sizeof(out->device_name));
    for (i = 0; out->device_name[i] != '\0'; i++) {
        if (out->device_name[i] == '-')
            out->device_name[i] = '_';
    }

    /* semver: major/minor/patch (base-10 stoi) + variant tag */
    {
        int major = manifest_stoi(fields[4]);
        int minor = manifest_stoi(fields[5]);
        int patch = manifest_stoi(fields[6]);
        int variant = manifest_variant_code(fields[7]);
        out->version = manifest_pack_version(major, minor, patch, variant);
    }

    /* flags default false; only parsed when the row carries those columns. */
    out->allowed_to_skip = false;
    out->skip_rollback = false;

    if (nfields >= 9) {                       /* size > 0x11f -> field[8] present */
        out->allowed_to_skip = (manifest_stoi(fields[8]) == 1);
        if (nfields >= 10) {                  /* (size >> 5) > 9 -> field[9] present */
            out->skip_rollback = (manifest_stoi(fields[9]) != 0);
        }
    }

    return true;
}

/*
 * OEM 0x00119210 — manifest_parse.
 *
 * Opens "<manifest_dir>/<manifest_name>" (both read from the update context at
 * +0x20/+0x28 and +0x40/+0x48), skips the header line, then parses every
 * subsequent line into a ManifestEntry appended to ctx->entries. After parsing
 * it logs each entry. On a failed read (getline sets EOF/fail) it stops, logs
 * the collected entries, closes the file and returns.
 */
void manifest_parse(UpdateContext *ctx)
{
    char path[256];
    void *fp;
    char line[512];
    size_t i;

    common_logf("devices/main/update/src/manifest.cpp", 0x21, LOG_INFO,
                "Parse manifest");

    /* path = ctx->manifest_dir + "/" + ctx->manifest_name */
    str_copy(path, ctx->manifest_dir, sizeof(path));
    str_append(path, "/", sizeof(path));
    str_append(path, ctx->manifest_name, sizeof(path));

    fp = manifest_open(path);   /* std::ifstream open, mode 8 (in|binary -> in) */

    /* Skip the header line (first getline, result ignored). */
    manifest_getline(fp, line, sizeof(line));

    /* Parse each subsequent line. getline returns false on EOF/fail. */
    while (manifest_getline(fp, line, sizeof(line))) {
        ManifestEntry entry;
        manifest_parse_row(&entry, ctx->manifest_dir, line);
        manifest_entries_push(&ctx->entries, &entry);
    }

    /* Log the collected entries. */
    for (i = 0; i < ctx->entries.count; i++) {
        const ManifestEntry *e = &ctx->entries.items[i];
        common_logf("devices/main/update/src/manifest.cpp", 0x2c, LOG_INFO,
                    "Manifest entry: %s - allowed_to_skip %s - skip_rollback %s",
                    e->image_path,
                    e->allowed_to_skip ? "true" : "false",
                    e->skip_rollback   ? "true" : "false");
    }

    manifest_close(fp);
}
