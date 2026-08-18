#ifndef DPYES_EXT_IMGUI_OVERLAY_H
#define DPYES_EXT_IMGUI_OVERLAY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install Direct3D 9 and Direct3D 11 render hooks through the already
 * initialized MinHook instance. Returns non-zero when at least one render
 * backend was hooked. */
int imgui_overlay_install_hooks(void);

#ifdef __cplusplus
}
#endif

#endif
