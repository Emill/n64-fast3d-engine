#if defined(ENABLE_DX11) || defined(ENABLE_DX12)

#ifndef GFX_DIRECT3D_COMMON_H
#define GFX_DIRECT3D_COMMON_H

#include <stdint.h>
#include <windows.h>

void ThrowIfFailed(HRESULT res);
void append_str(char *buf, size_t *len, const char *str);
void append_line(char *buf, size_t *len, const char *str);
const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element);
void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix, bool with_alpha, bool only_alpha, bool opt_alpha);

#endif

#endif
