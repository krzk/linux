#!/bin/awk -f
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2025, Oracle and/or its affiliates.
#
# Usage: awk -f gen-cpu-feature-names-x86.awk cpufeatures.h > cpu-feature-names.c
#

BEGIN {
	print "/* cpu feature name array generated from cpufeatures.h */"
	print "/* Do not change this code. */"
	print
	print "static const char *cpu_feature_names[(NCAPINTS+NBUGINTS)*32] = {"

	feature_expr = "(X86_FEATURE_[A-Z0-9_]+)\\s+\\(([0-9*+ ]+)\\)"
	debug_expr = "(X86_BUG_[A-Z0-9_]+)\\s+X86_BUG\\(([0-9*+ ]+)\\)"
}

/^#define X86_FEATURE_/ {
	if (match($0, feature_expr, m)) {
		print "\t[" m[2] "] = \"" m[1] "\","
	}
}

/^#define X86_BUG_/ {
	if (match($0, debug_expr, m)) {
		print "\t[NCAPINTS*32+(" m[2] ")] = \"" m[1] "\","
	}
}

END {
	print "};"
}
