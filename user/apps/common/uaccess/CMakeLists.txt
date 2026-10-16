# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# One image per access, each ending the system when the access faults. The composition gives
# main the whole authority word, which the witness call demands.
set(_uaccess_op_read KOS_WITNESS_UREAD_NOW)
set(_uaccess_op_write KOS_WITNESS_UWRITE_NOW)
set(_uaccess_op_park KOS_WITNESS_UREAD_AFTER_PARK)
set(_uaccess_op_spaceless KOS_WITNESS_UREAD_SPACELESS)
set(_uaccess_op_fault KOS_WITNESS_UREAD_AT_FAULT_EXIT)
set(_uaccess_op_fetch KOS_WITNESS_UFETCH_NOW)
set(_uaccess_op_irq KOS_WITNESS_UREAD_IN_TIMER_IRQ)
foreach(_arm read write park spaceless fault fetch irq)
  add_executable(uaccess_${_arm} main.cc)
  kickos_app_system(uaccess_${_arm} COMPOSITION system.yaml SYSTEM uaccess_system)
  target_compile_definitions(uaccess_${_arm} PRIVATE KICKOS_UACCESS_OP=${_uaccess_op_${_arm}})
  kickos_emit_image(uaccess_${_arm})
endforeach()
