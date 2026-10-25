/*
 * xbe_variant.c -- g_recomp_xbe_variant: which edition's branch conditions
 * the lifted code takes where editions of a disc differ only there
 * (tools/recomp --edition-variants; recomp_types.h). 0 = the lifted disc's;
 * the title sets it at boot from the disc it identified.
 */
int g_recomp_xbe_variant = 0;
