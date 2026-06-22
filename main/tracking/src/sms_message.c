/*
 * sms_message.c -- VanMoof i.MX8 tracking service: inbound SMS command parser.
 *
 * Behaviour-oriented C reconstruction (C++ modelled in C). The framework
 * objects (nlohmann::json, std::string, libstdc++) are VENDOR and modelled as
 * opaque handles + extern prototypes in "tracking_common.h".
 *
 * OEM source path (from common_logf): devices/main/tracking/src/sms_message.cpp
 */

#include "tracking_common.h"

/*
 * sms_message_parse  @ 0x115d50
 *
 * Parses an inbound SMS JSON (delivered on the "modem/sms" topic) into a
 * tracking command code.
 *
 * Logic (translated from the decompiled body):
 *   - Look up the object key "tracking" in the message (json find/operator[]).
 *   - If the message is not an object / the key is absent / the lookup throws
 *     (json parse error path), log:
 *         sms_message.cpp:0x25 (37), level 2 (INFO):
 *         "Could not parse sms message : %s"
 *     with the offending value's dump, and return -1 (0xFFFFFFFF).
 *   - Otherwise compare the "tracking" value (json operator==, FUN_00116870)
 *     against the string literals:
 *         "enabled"  -> return  0   (command_thread maps this to state THEFT)
 *         "disabled" -> return  1   (command_thread maps this to state OFF)
 *         anything else / missing -> return -1
 *
 * NOTE: this build returns only 0 / 1 / -1; it never returns 2. The "locate"
 * command (code 2) handled by the command thread originates elsewhere, not
 * from this parser.
 */
int sms_message_parse(const json_t *msg)
{
    json_t tracking_val; /* "tracking" sub-value, by value (json copy) */
    int result;

    /* msg["tracking"] -- if msg is not an object this throws type_error.302
       ("cannot use operator[] with a string argument with <type>"), which is
       caught and reported as a parse failure below. */
    if (!json_is_object(msg)) {
        /* operator[] on a non-object: report and bail. */
        char *dump = json_dump(msg, -1);
        common_logf("devices/main/tracking/src/sms_message.cpp", 0x25, 2,
                    "Could not parse sms message : %s", dump);
        json_dump_free(dump);
        return -1;
    }

    if (!json_find(msg, "tracking", &tracking_val)) {
        /* Key absent: nothing to act on. */
        return -1;
    }

    if (json_equals_string(&tracking_val, "enabled")) {
        result = 0;            /* -> set tracking state THEFT */
    } else if (json_equals_string(&tracking_val, "disabled")) {
        result = 1;            /* -> set tracking state OFF */
    } else {
        result = -1;           /* unrecognised value */
    }

    json_free(&tracking_val);
    return result;
}
