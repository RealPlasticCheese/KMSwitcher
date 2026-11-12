#pragma once
// Native RAM supports bitfield accesses. Actual SoC/LL register layouts and
// read/clear functions remain upstream code; this replaces GCC-only typeof.
#define HAL_FORCE_MODIFY_U32_REG_FIELD(reg, field, value) ((reg).field = (value))
#define HAL_FORCE_READ_U32_REG_FIELD(reg, field) ((reg).field)
