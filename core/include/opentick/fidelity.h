/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file fidelity.h
 * @brief The fidelity contract (docs/bridge-protocol.md §7): what a machine
 *        requires of its signals, what a bridge guarantees, and the check of
 *        one against the other before a run.
 *
 *     OtkFidelityFile machine, bridge;
 *     OtkFidelityError e;
 *     otk_fidelity_parse(text, len, &machine, &e);    // each file, from memory
 *     OtkFidelityResult r;
 *     if (otk_fidelity_check(&machine, &bridge, &r) != OTK_FIDELITY_OK) {
 *         char msg[256];
 *         otk_fidelity_describe(&r, &machine, &bridge, msg, sizeof(msg));
 *     }
 *
 * The rules (§7):
 * - A machine row takes, in file order, the next bridge row not yet taken
 *   with the same direction and transport. None left: refused.
 * - An empty machine cell requires nothing. A machine cell set against an
 *   empty bridge cell is refused: not guaranteed.
 * - max_rate_hz: machine <= bridge. resolution_ns, jitter_ns: bridge <=
 *   machine. logic_v: equal.
 * - latency_ns: for to_dut, the bridge row's own; for from_dut, the
 *   bridge's dut_to_plant_delay_ns (metadata) plus its row's latency_ns
 *   (empty counts 0). Bridge <= machine.
 * - The first failure is returned: machine rows in order, columns in the
 *   header's order.
 *
 * ISO C, no allocation: a file is held in a fixed struct.
 */

#ifndef OPENTICK_FIDELITY_H
#define OPENTICK_FIDELITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The version of the format this parser reads (the metadata line). */
#define OTK_FIDELITY_FORMAT_VERSION 1
#define OTK_FIDELITY_MAX_SIGNALS 64
#define OTK_FIDELITY_NAME_MAX 32 /**< with the terminating NUL */

typedef enum { OTK_FIDELITY_SIDE_MACHINE = 0, OTK_FIDELITY_SIDE_BRIDGE } OtkFidelitySide;
typedef enum { OTK_FIDELITY_TO_DUT = 0, OTK_FIDELITY_FROM_DUT } OtkFidelityDir;
typedef enum {
    OTK_FIDELITY_ROTARY = 0,
    OTK_FIDELITY_CAPTURE,
    OTK_FIDELITY_ANALOG,
    OTK_FIDELITY_LEVEL
} OtkFidelityTransport;

/** @brief The numeric columns, in the header's order. */
typedef enum {
    OTK_FIDELITY_MAX_RATE_HZ = 0,
    OTK_FIDELITY_RESOLUTION_NS,
    OTK_FIDELITY_LATENCY_NS,
    OTK_FIDELITY_JITTER_NS,
    OTK_FIDELITY_LOGIC_MV, /**< the file's logic_v, held in millivolts */
    OTK_FIDELITY_NUM_COLUMNS
} OtkFidelityColumn;

typedef struct {
    char name[OTK_FIDELITY_NAME_MAX];
    OtkFidelityDir dir;
    OtkFidelityTransport transport;
    bool has[OTK_FIDELITY_NUM_COLUMNS]; /**< false: an empty cell */
    uint64_t value[OTK_FIDELITY_NUM_COLUMNS];
    unsigned line; /**< in its file, from 1 */
} OtkFidelitySignal;

typedef struct {
    OtkFidelitySide side;
    bool has_lead, has_delay;
    uint64_t lead_ns;               /**< metadata lead_ns (bridge) */
    uint64_t dut_to_plant_delay_ns; /**< metadata dut_to_plant_delay_ns (bridge) */
    char link[OTK_FIDELITY_NAME_MAX];
    size_t n;
    OtkFidelitySignal sig[OTK_FIDELITY_MAX_SIGNALS];
} OtkFidelityFile;

/** @brief Where a file could not be read. */
typedef struct {
    unsigned line;      /**< from 1 */
    const char *column; /**< the column or metadata key, or NULL for the whole line */
    const char *what;   /**< a static description */
} OtkFidelityError;

/**
 * @brief Reads one file from memory (it need not end in a NUL). Lines end in
 *        "\n" or "\r\n"; blank lines after the last row are ignored. Metadata
 *        keys other than side, lead_ns, dut_to_plant_delay_ns and link are
 *        ignored.
 * @return false, with @p err filled, if the file is not a valid version 1 file
 */
bool otk_fidelity_parse(const char *text, size_t len, OtkFidelityFile *f, OtkFidelityError *err);

typedef enum {
    OTK_FIDELITY_OK = 0,
    OTK_FIDELITY_WRONG_SIDE,      /**< the machine file is not side=machine, or the bridge's not side=bridge */
    OTK_FIDELITY_NO_CHANNEL,      /**< no bridge row left with that direction and transport */
    OTK_FIDELITY_NOT_GUARANTEED,  /**< required, and the bridge's cell is empty */
    OTK_FIDELITY_NOT_MET          /**< required, and the bridge's number does not meet it */
} OtkFidelityVerdict;

typedef struct {
    OtkFidelityVerdict verdict;
    size_t machine_row;       /**< index of the signal that failed */
    size_t bridge_row;        /**< the bridge row it took (NOT_GUARANTEED, NOT_MET) */
    OtkFidelityColumn column; /**< NOT_GUARANTEED, NOT_MET */
    uint64_t required;        /**< the machine's number */
    uint64_t offered;         /**< the bridge's, as compared (latency: with the delay) */
} OtkFidelityResult;

/** @brief Checks a machine's file against a bridge's (rules above). */
OtkFidelityVerdict otk_fidelity_check(const OtkFidelityFile *machine, const OtkFidelityFile *bridge,
                                      OtkFidelityResult *r);

/** @brief The column's name as in the file's header ("logic_v" for the millivolts). */
const char *otk_fidelity_column_name(OtkFidelityColumn c);

/**
 * @brief One line for a person: the signal, its line and the column, both
 *        numbers. Always NUL-terminated when @p cap > 0.
 * @return the length it needed, as snprintf
 */
int otk_fidelity_describe(const OtkFidelityResult *r, const OtkFidelityFile *machine, const OtkFidelityFile *bridge,
                          char *buf, size_t cap);

#endif /* OPENTICK_FIDELITY_H */
