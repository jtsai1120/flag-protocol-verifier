OPENQASM 3.0;
include "stdgates.inc";

// Liou & Lai, arXiv:2407.00607, Fig. 9.
// [[5,1,3]] sequential [2,2] flag-sharing full syndrome extraction.
//
// Measured stabilizers, in generalized-syndrome order m1534:
//   g1 = X Z Z X I
//   g5 = Z Z X I X
//   g3 = X I X Z Z
//   g4 = Z X I X Z
//
// Part (a): g1 and g3 share one flag qubit.
// Part (b): g5 and g4 share one flag qubit.
// The same two measurement qubits and one flag qubit are reset/reused.
// Each part has two-qubit depth 7; total flagged depth = 14.
// Total two-qubit gates = 24.
//
// data[0..4] = physical data qubits 1..5.
// syn[0],syn[1] = the two measurement ancillas used in each part.
// flag[0] = the shared flag ancilla, reused between parts.
// m[0..3] = (m1,m5,m3,m4).
// f[0..1] = (f1,f2).

qubit[5] data;
qubit[2] syn;
qubit[1] flag;
bit[4] m;
bit[2] f;

// ============================================================
// Fig. 9(a): g1 (syn[0] -> m1) and g3 (syn[1] -> m3), shared f1.
// ============================================================
reset syn[0];
h syn[0];
reset syn[1];
h syn[1];
reset flag[0];

// depth 1: g1:Z2, g3:X3
cz syn[0], data[1];
cx syn[1], data[2];

// depth 2: a = first g1--f1 coupling
cx syn[0], flag[0];

// depth 3: g1:Z3, b = first g3--f1 coupling
cz syn[0], data[2];
cx syn[1], flag[0];

// depth 4: g1:X4, g3:Z5
cx syn[0], data[3];
cz syn[1], data[4];

// depth 5: c = second g1--f1 coupling, g3:X1
cx syn[0], flag[0];
cx syn[1], data[0];

// depth 6: g1:X1, d = second g3--f1 coupling
cx syn[0], data[0];
cx syn[1], flag[0];

// depth 7: g3:Z4
cz syn[1], data[3];

// Measure m1,m3 in X basis and f1 in Z basis.
h syn[0];
m[0] = measure syn[0];
h syn[1];
m[2] = measure syn[1];
f[0] = measure flag[0];

// ============================================================
// Fig. 9(b): g5 (syn[0] -> m5) and g4 (syn[1] -> m4), shared f2.
// ============================================================
reset syn[0];
h syn[0];
reset syn[1];
h syn[1];
reset flag[0];

// depth 1: g5:Z1, g4:X4
cz syn[0], data[0];
cx syn[1], data[3];

// depth 2: e = first g5--f2 coupling
cx syn[0], flag[0];

// depth 3: g5:Z2, f = first g4--f2 coupling
cz syn[0], data[1];
cx syn[1], flag[0];

// depth 4: g5:X3, g4:Z1
cx syn[0], data[2];
cz syn[1], data[0];

// depth 5: g = second g5--f2 coupling, g4:X2
cx syn[0], flag[0];
cx syn[1], data[1];

// depth 6: g5:X5, h = second g4--f2 coupling
cx syn[0], data[4];
cx syn[1], flag[0];

// depth 7: g4:Z5
cz syn[1], data[4];

// Measure m5,m4 in X basis and f2 in Z basis.
h syn[0];
m[1] = measure syn[0];
h syn[1];
m[3] = measure syn[1];
f[1] = measure flag[0];
