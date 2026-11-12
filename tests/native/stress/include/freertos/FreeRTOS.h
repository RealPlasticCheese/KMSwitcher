#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define portENTER_CRITICAL_SAFE(p) portENTER_CRITICAL(p)
#define portEXIT_CRITICAL_SAFE(p) portEXIT_CRITICAL(p)
