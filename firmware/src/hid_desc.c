// HID report descriptor parser. See hid_desc.h.
#include "hid_desc.h"

#include <string.h>

#define MAX_REPORTS 32
#define MAX_USAGES 32
#define MAX_STACK 4
#define MAX_REPORT_BITS (8u * 1024u)

typedef struct {
    uint16_t usage_page;
    int32_t logical_min;
    uint32_t logical_max_u;
    int32_t logical_max_s;
    uint8_t report_size;
    uint8_t report_id;
    uint16_t report_count;
} globals_t;

typedef struct {
    uint8_t id;
    uint8_t type;
    uint32_t bits;
} report_pos_t;

typedef struct {
    uint32_t usages[MAX_USAGES];  // high 16 bits: page (0 = use the current usage page)
    uint8_t n_usages;
    bool has_min, has_max;
    uint32_t usage_min, usage_max;
} locals_t;

static uint32_t item_udata(const uint8_t* p, uint8_t size) {
    switch (size) {
        case 1: return p[0];
        case 2: return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
        case 4: return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
        default: return 0;
    }
}

static int32_t item_sdata(const uint8_t* p, uint8_t size) {
    switch (size) {
        case 1: return (int8_t) p[0];
        case 2: return (int16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
        case 4: return (int32_t) item_udata(p, 4);
        default: return 0;
    }
}

static report_pos_t* report_pos(report_pos_t* reports, uint8_t* n_reports, uint8_t id, uint8_t type) {
    for (uint8_t i = 0; i < *n_reports; i++) {
        if (reports[i].id == id && reports[i].type == type) {
            return &reports[i];
        }
    }
    if (*n_reports >= MAX_REPORTS) {
        return NULL;
    }
    report_pos_t* r = &reports[(*n_reports)++];
    r->id = id;
    r->type = type;
    r->bits = 0;
    return r;
}

static uint32_t full_usage(uint32_t u, uint16_t page) {
    return (u & 0xFFFF0000u) ? u : (((uint32_t) page << 16) | (u & 0xFFFFu));
}

bool hid_desc_walk(const uint8_t* desc, uint16_t len, hid_element_cb_t cb, void* ctx) {
    globals_t g;
    globals_t stack[MAX_STACK];
    uint8_t sp = 0;
    locals_t l;
    report_pos_t reports[MAX_REPORTS];
    uint8_t n_reports = 0;

    memset(&g, 0, sizeof(g));
    memset(&l, 0, sizeof(l));

    uint16_t i = 0;
    while (i < len) {
        uint8_t prefix = desc[i];
        if (prefix == 0xFE) {  // long item
            if (i + 2 >= len) {
                return false;
            }
            uint16_t skip = (uint16_t) (3u + desc[i + 1]);
            if ((uint32_t) i + skip > len) {
                return false;
            }
            i = (uint16_t) (i + skip);
            continue;
        }
        uint8_t size = prefix & 0x03;
        if (size == 3) {
            size = 4;
        }
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0F;
        if ((uint32_t) i + 1u + size > len) {
            return false;
        }
        const uint8_t* data = &desc[i + 1];
        uint32_t ud = item_udata(data, size);
        int32_t sd = item_sdata(data, size);
        i = (uint16_t) (i + 1u + size);

        if (type == 1) {  // global
            switch (tag) {
                case 0x0: g.usage_page = (uint16_t) ud; break;
                case 0x1: g.logical_min = sd; break;
                case 0x2: g.logical_max_u = ud; g.logical_max_s = sd; break;
                case 0x7: g.report_size = (uint8_t) ud; break;
                case 0x8: g.report_id = (uint8_t) ud; break;
                case 0x9: g.report_count = (uint16_t) ud; break;
                case 0xA:  // push
                    if (sp >= MAX_STACK) {
                        return false;
                    }
                    stack[sp++] = g;
                    break;
                case 0xB:  // pop
                    if (sp == 0) {
                        return false;
                    }
                    g = stack[--sp];
                    break;
                default: break;  // physical min/max, unit, unit exponent
            }
        } else if (type == 2) {  // local
            switch (tag) {
                case 0x0:
                    if (l.n_usages < MAX_USAGES) {
                        l.usages[l.n_usages++] = (size == 4) ? ud : (ud & 0xFFFFu);
                    }
                    break;
                case 0x1: l.usage_min = (size == 4) ? ud : (ud & 0xFFFFu); l.has_min = true; break;
                case 0x2: l.usage_max = (size == 4) ? ud : (ud & 0xFFFFu); l.has_max = true; break;
                default: break;
            }
        } else if (type == 0) {  // main
            if (tag == 0x8 || tag == 0x9 || tag == 0xB) {
                hid_rpt_type_t rt = (tag == 0x8) ? HID_RPT_INPUT : (tag == 0x9) ? HID_RPT_OUTPUT : HID_RPT_FEATURE;
                report_pos_t* rp = report_pos(reports, &n_reports, g.report_id, (uint8_t) rt);
                if (rp == NULL) {
                    return false;
                }
                // Same rule as Linux hid-core: the maximum is signed only if the minimum is negative.
                int32_t lmax = (g.logical_min < 0) ? g.logical_max_s
                                                   : (int32_t) ((g.logical_max_u > 0x7FFFFFFFu) ? 0x7FFFFFFFu : g.logical_max_u);
                uint32_t total = (uint32_t) g.report_size * g.report_count;
                if (g.report_size > 32 || rp->bits + total > MAX_REPORT_BITS) {
                    return false;
                }
                bool variable = (ud & HID_FLAG_VARIABLE) != 0;
                for (uint16_t e = 0; e < g.report_count; e++) {
                    uint32_t u = 0;
                    if (l.n_usages > 0) {
                        u = l.usages[(e < l.n_usages) ? e : (uint8_t) (l.n_usages - 1)];
                    } else if (l.has_min) {
                        uint32_t umax = l.has_max ? l.usage_max : l.usage_min;
                        u = variable ? l.usage_min + e : l.usage_min;
                        if ((u & 0xFFFFu) > (umax & 0xFFFFu)) {
                            u = umax;
                        }
                    }
                    u = full_usage(u, g.usage_page);
                    if (cb != NULL && g.report_size > 0) {
                        hid_element_t el = {
                            .type = rt,
                            .report_id = g.report_id,
                            .usage_page = (uint16_t) (u >> 16),
                            .usage = (uint16_t) (u & 0xFFFFu),
                            .bit_offset = (uint16_t) (rp->bits + (uint32_t) e * g.report_size),
                            .bit_size = g.report_size,
                            .flags = (uint16_t) ud,
                            .logical_min = g.logical_min,
                            .logical_max = lmax,
                        };
                        cb(ctx, &el);
                    }
                }
                rp->bits += total;
            }
            memset(&l, 0, sizeof(l));  // collection, end collection and data items all clear locals
        }
        // type 3 is reserved; ignored
    }
    return true;
}

// ---------------------------------------------------------------- queries

typedef struct {
    hid_rpt_type_t type;
    uint16_t page, usage;
    hid_field_t* out;
} find_ctx_t;

static void find_cb(void* vctx, const hid_element_t* el) {
    find_ctx_t* c = (find_ctx_t*) vctx;
    if (c->out->present || el->type != c->type || el->usage_page != c->page || el->usage != c->usage) {
        return;
    }
    if (!(el->flags & HID_FLAG_VARIABLE) || (el->flags & HID_FLAG_CONSTANT)) {
        return;
    }
    c->out->present = true;
    c->out->report_id = el->report_id;
    c->out->bit_offset = el->bit_offset;
    c->out->bit_size = el->bit_size;
    c->out->logical_min = el->logical_min;
    c->out->logical_max = el->logical_max;
    c->out->flags = el->flags;
}

bool hid_desc_find_usage(const uint8_t* desc, uint16_t len, hid_rpt_type_t type,
    uint16_t usage_page, uint16_t usage, hid_field_t* out) {
    memset(out, 0, sizeof(*out));
    find_ctx_t c = { type, usage_page, usage, out };
    hid_desc_walk(desc, len, find_cb, &c);
    return out->present;
}

typedef struct {
    hid_rpt_type_t type;
    uint8_t id;
    uint32_t bits;
} bits_ctx_t;

static void bits_cb(void* vctx, const hid_element_t* el) {
    bits_ctx_t* c = (bits_ctx_t*) vctx;
    if (el->type == c->type && el->report_id == c->id) {
        uint32_t end = (uint32_t) el->bit_offset + el->bit_size;
        if (end > c->bits) {
            c->bits = end;
        }
    }
}

uint32_t hid_desc_report_bits(const uint8_t* desc, uint16_t len, hid_rpt_type_t type, uint8_t report_id) {
    bits_ctx_t c = { type, report_id, 0 };
    hid_desc_walk(desc, len, bits_cb, &c);
    return c.bits;
}

typedef struct {
    uint8_t id;
    uint16_t* offsets;
    uint16_t max;
    uint16_t highest;
} btn_ctx_t;

static void btn_cb(void* vctx, const hid_element_t* el) {
    btn_ctx_t* c = (btn_ctx_t*) vctx;
    if (el->type != HID_RPT_INPUT || el->report_id != c->id || el->usage_page != HID_PAGE_BUTTON) {
        return;
    }
    if (!(el->flags & HID_FLAG_VARIABLE) || (el->flags & HID_FLAG_CONSTANT) || el->bit_size != 1) {
        return;
    }
    if (el->usage == 0 || el->usage > c->max) {
        return;
    }
    if (c->offsets[el->usage - 1] == 0xFFFF) {
        c->offsets[el->usage - 1] = el->bit_offset;
        if (el->usage > c->highest) {
            c->highest = el->usage;
        }
    }
}

uint16_t hid_desc_button_offsets(const uint8_t* desc, uint16_t len, uint8_t report_id,
    uint16_t* offsets, uint16_t max_buttons) {
    for (uint16_t i = 0; i < max_buttons; i++) {
        offsets[i] = 0xFFFF;
    }
    btn_ctx_t c = { report_id, offsets, max_buttons, 0 };
    hid_desc_walk(desc, len, btn_cb, &c);
    return c.highest;
}

// ---------------------------------------------------------------- extraction

bool hid_extract_bits(const uint8_t* payload, uint16_t payload_len, uint16_t bit_offset, uint8_t bit_size, uint32_t* value) {
    if (bit_size == 0 || bit_size > 32 || (uint32_t) bit_offset + bit_size > (uint32_t) payload_len * 8u) {
        return false;
    }
    uint32_t v = 0;
    for (uint8_t i = 0; i < bit_size; i++) {
        uint32_t b = (uint32_t) bit_offset + i;
        if ((payload[b >> 3] >> (b & 7u)) & 1u) {
            v |= 1u << i;
        }
    }
    *value = v;
    return true;
}

bool hid_extract(const uint8_t* payload, uint16_t payload_len, const hid_field_t* f, int32_t* value) {
    uint32_t raw;
    if (!f->present || !hid_extract_bits(payload, payload_len, f->bit_offset, f->bit_size, &raw)) {
        return false;
    }
    if (f->logical_min < 0 && f->bit_size < 32 && ((raw >> (f->bit_size - 1)) & 1u)) {
        raw |= ~0u << f->bit_size;
    }
    *value = (int32_t) raw;
    return true;
}
