#include <cosmos/acpi_s5.hpp>
#include <stdio.h>

static int g_fail = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++g_fail; } } while (0)

static bool find(const unsigned char* b, unsigned n, int* a, int* t) {
    u8 x = 0xFF, y = 0xFF;
    bool ok = acpi::find_s5(b, n, &x, &y);
    *a = x; *t = y;
    return ok;
}

int main() {
    int a, b;
    {   // What QEMU q35 emits: Name (_S5, Package (0x04) { Zero, Zero, Zero, Zero })
        const unsigned char aml[] = {0x10, 0x20, 0x5C, 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00};
        CHECK(find(aml, sizeof aml, &a, &b) && a == 0 && b == 0, "zero-valued package");
    }
    {   // Typical firmware: Package (0x04) { 0x05, 0x05, 0x00, 0x00 } with byte prefixes
        const unsigned char aml[] = {0x08, '_', 'S', '5', '_', 0x12, 0x0B, 0x04, 0x0A, 0x05, 0x0A, 0x07, 0x0A, 0x00, 0x0A, 0x00};
        CHECK(find(aml, sizeof aml, &a, &b) && a == 5 && b == 7, "byte-prefixed package");
    }
    {   // One, One are bare constants; a word prefix is accepted too
        const unsigned char aml[] = {0x08, '_', 'S', '5', '_', 0x12, 0x07, 0x02, 0x01, 0x0B, 0x03, 0x00};
        CHECK(find(aml, sizeof aml, &a, &b) && a == 1 && b == 3, "bare and word constants");
    }
    {   // Two-byte PkgLength
        const unsigned char aml[] = {0x08, '_', 'S', '5', '_', 0x12, 0x41, 0x06, 0x04, 0x0A, 0x02, 0x0A, 0x03, 0x00, 0x00};
        CHECK(find(aml, sizeof aml, &a, &b) && a == 2 && b == 3, "two-byte package length");
    }
    {   // The name appearing inside something that is not a Name definition is rejected
        const unsigned char aml[] = {0x14, 0x09, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A, 0x05, 0x0A, 0x05};
        CHECK(!find(aml, sizeof aml, &a, &b), "no NameOp before the name");
    }
    {   // A false hit first, the real object after it
        const unsigned char aml[] = {'_', 'S', '5', '_', 0x00, 0x00, 0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A, 0x04, 0x0A, 0x04};
        CHECK(find(aml, sizeof aml, &a, &b) && a == 4 && b == 4, "scan continues past a false match");
    }
    {   // A SLP_TYP larger than three bits, and a truncated table
        const unsigned char big[] = {0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A, 0x09, 0x0A, 0x00};
        CHECK(!find(big, sizeof big, &a, &b), "SLP_TYP above 7 refused");
        const unsigned char cut[] = {0x08, '_', 'S', '5', '_', 0x12, 0x06, 0x04, 0x0A};
        CHECK(!find(cut, sizeof cut, &a, &b), "truncated package");
        const unsigned char none[] = {0x10, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        CHECK(!find(none, sizeof none, &a, &b), "no _S5_ at all");
        CHECK(!find(none, 3, &a, &b), "tiny table");
    }
    if (g_fail) return 1;
    printf("acpi: all checks passed\n");
    return 0;
}
