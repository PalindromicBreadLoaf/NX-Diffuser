// G-Diffuser — port entry point.
// Slice 4b stage 1: a stub main linked against the decomp game object lib, with NO
// libultraship yet. The resulting unresolved-symbol set is the authoritative "shim
// surface" — exactly what libultraship must provide vs. what the port reimplements (4c).
// Real Ship::Context init + generic.o2r loading + resource factories land next.

int main(int argc, char** argv) {
    return 0;
}
