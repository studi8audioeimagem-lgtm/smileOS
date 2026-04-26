#include "kernel.h"
#include "types.h"
#include "string.h"

extern void isr_stub_0(void);
extern void isr_stub_1(void);
extern void isr_stub_2(void);
extern void isr_stub_3(void);
extern void isr_stub_4(void);
extern void isr_stub_5(void);
extern void isr_stub_6(void);
extern void isr_stub_7(void);
extern void isr_stub_8(void);
extern void isr_stub_9(void);
extern void isr_stub_10(void);
extern void isr_stub_11(void);
extern void isr_stub_12(void);
extern void isr_stub_13(void);
extern void isr_stub_14(void);
extern void isr_stub_15(void);
extern void isr_stub_16(void);
extern void isr_stub_17(void);
extern void isr_stub_18(void);
extern void isr_stub_19(void);
extern void isr_stub_20(void);
extern void isr_stub_21(void);
extern void isr_stub_22(void);
extern void isr_stub_23(void);
extern void isr_stub_24(void);
extern void isr_stub_25(void);
extern void isr_stub_26(void);
extern void isr_stub_27(void);
extern void isr_stub_28(void);
extern void isr_stub_29(void);
extern void isr_stub_30(void);
extern void isr_stub_31(void);

typedef struct {
    u16 off_low;
    u16 sel;
    u8 ist;
    u8 type_attr;
    u16 off_mid;
    u32 off_high;
    u32 zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) idtr_t;

static idt_entry_t g_idt[256] __attribute__((aligned(16)));
static idtr_t g_idtr;

static void idt_set_gate(u8 vec, void (*fn)(void)) {
    const u64 addr = (u64)(usize)fn;
    g_idt[vec].off_low = (u16)(addr & 0xFFFF);
    g_idt[vec].sel = 0x08;
    g_idt[vec].ist = 0;
    g_idt[vec].type_attr = 0x8E; /* present, ring0, interrupt gate */
    g_idt[vec].off_mid = (u16)((addr >> 16) & 0xFFFF);
    g_idt[vec].off_high = (u32)((addr >> 32) & 0xFFFFFFFF);
    g_idt[vec].zero = 0;
}

static void lidt(const idtr_t *idtr) {
    __asm__ volatile("lidt (%0)" : : "r"(idtr) : "memory");
}

static void hex64(char out[17], u64 v) {
    static const char *h = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        const int shift = (15 - i) * 4;
        out[i] = h[(v >> shift) & 0xFULL];
    }
    out[16] = 0;
}

__attribute__((noinline, optimize("O0")))
void isr_exception_handler(u64 vector, u64 error, u64 rip, u64 cr2) {
    log_write("EXCEPTION");

    char buf[48];
    buf[0] = 'V'; buf[1] = 'E'; buf[2] = 'C'; buf[3] = ':'; buf[4] = ' ';
    buf[5] = (char)('0' + (char)((vector / 10ULL) % 10ULL));
    buf[6] = (char)('0' + (char)(vector % 10ULL));
    buf[7] = 0;
    log_write(buf);

    char h16[17];
    hex64(h16, rip);
    char line1[40];
    line1[0] = 'R'; line1[1] = 'I'; line1[2] = 'P'; line1[3] = ':'; line1[4] = ' ';
    for (int i = 0; i < 16; i++) line1[5 + i] = h16[i];
    line1[21] = 0;
    log_write(line1);

    hex64(h16, error);
    char line2[40];
    line2[0] = 'E'; line2[1] = 'R'; line2[2] = 'R'; line2[3] = ':'; line2[4] = ' ';
    for (int i = 0; i < 16; i++) line2[5 + i] = h16[i];
    line2[21] = 0;
    log_write(line2);

    /* Always log CR2 as well (helps even for non-pagefault crashes). */
    hex64(h16, cr2);
    char line3[40];
    line3[0] = 'C'; line3[1] = 'R'; line3[2] = '2'; line3[3] = ':'; line3[4] = ' ';
    for (int i = 0; i < 16; i++) line3[5 + i] = h16[i];
    line3[21] = 0;
    log_write(line3);

    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void idt_init(void) {
    /* Keep this init path very simple (no vectorized clears / large stack frames). */
    kmemset(g_idt, 0, sizeof(g_idt));

    static void (*const stubs[32])(void) __attribute__((aligned(16))) = {
        isr_stub_0, isr_stub_1, isr_stub_2, isr_stub_3,
        isr_stub_4, isr_stub_5, isr_stub_6, isr_stub_7,
        isr_stub_8, isr_stub_9, isr_stub_10, isr_stub_11,
        isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15,
        isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19,
        isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23,
        isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27,
        isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31
    };

    for (u8 v = 0; v < 32; v++) {
        idt_set_gate(v, stubs[v]);
    }

    g_idtr.limit = (u16)(sizeof(g_idt) - 1);
    g_idtr.base = (u64)(usize)&g_idt[0];
    lidt(&g_idtr);
    log_write("cpu: idt installed (exceptions 0-31)");
}
