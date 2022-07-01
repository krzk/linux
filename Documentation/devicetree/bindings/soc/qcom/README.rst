.. SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause

Qualcomm SoC compatibles naming convention
==========================================
1. When adding new compatibles in new bindings, use the format:
   ::

     qcom,SoC-IP

   For example:
   ::

     qcom,sdm845-llcc-bwmon

2. When adding new compatibles to existing bindings, use the format
   in the existing binding, even if it contradicts the above.
