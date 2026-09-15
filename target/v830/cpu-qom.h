#ifndef TARGET_V830_CPU_QOM_H
#define TARGET_V830_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_V830_CPU "v830-cpu"
OBJECT_DECLARE_CPU_TYPE(V830CPU, V830CPUClass, V830_CPU) // self-explanatory

#define V830_CPU_TYPE_SUFFIX "-" TYPE_V830_CPU // naming convention
#define V830_CPU_TYPE_NAME(name) (name V830_CPU_TYPE_SUFFIX) // for use in machine definitions

#endif
