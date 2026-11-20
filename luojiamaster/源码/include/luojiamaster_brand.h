#ifndef LUOJIAMASTER_BRAND_H
#define LUOJIAMASTER_BRAND_H

// Shared by the client UI and the Windows resource compiler.
#define LUOJIAMASTER_PRODUCT "luojiamaster"
#define LUOJIAMASTER_TEAM "luojiafox 珞珈狐战队"
#define LUOJIAMASTER_AUTHOR "made by CH"

#ifdef __cplusplus
#define LUOJIAMASTER_WIDEN_IMPL(text) L##text
#define LUOJIAMASTER_WIDEN(text) LUOJIAMASTER_WIDEN_IMPL(text)
#define LUOJIAMASTER_PRODUCT_W LUOJIAMASTER_WIDEN(LUOJIAMASTER_PRODUCT)
#endif

#endif
