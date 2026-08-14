// On-screen keyboard for ImGui text fields on platforms that have one.

#ifdef __cplusplus
extern "C" {
#endif

// Raise the on-screen keyboard when an ImGui text field takes focus, and dismiss it when the field
// is dismissed.
void gdx_swkbd_tick(void);

#ifdef __cplusplus
}
#endif
