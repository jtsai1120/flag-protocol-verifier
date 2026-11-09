OPENQASM 3.0;
include "stdgates.inc";

// Liou & Lai, arXiv:2407.00607, Fig. 10.
// [[5,1,3]] fully-parallel [2,2]^T flag-sharing full syndrome extraction.
// (Paper notation: column vector [2;2].)
//
// Measured stabilizers, in generalized-syndrome order m1534:
//   g1 = X Z Z X I   -> syn[0], flag[0]
//   g5 = Z Z X I X   -> syn[2], flag[1]
//   g3 = X I X Z Z   -> syn[1], flag[0]
//   g4 = Z X I X Z   -> syn[3], flag[1]
//
// g1 and g3 share f1; g5 and g4 share f2.
// Two-qubit depth = 7, total two-qubit gates = 24.
// Uses 4 measurement ancillas + 2 shared flag ancillas = 6 ancillas.
//
// data[0..4] = physical data qubits 1..5.
// syn[0..3] = (m1,m3,m5,m4) physically.
// m[0..3] = (m1,m5,m3,m4) logically.
// flag[0..1] = (f1,f2).

qubit[5] data;
qubit[4] syn;
qubit[2] flag;
bit[4] m;
bit[2] f;

// Prepare all measurement ancillas in |+>.
reset syn[0];
h syn[0];
reset syn[1];
h syn[1];
reset syn[2];
h syn[2];
reset syn[3];
h syn[3];

// Prepare flags in |0>.
reset flag[0];
reset flag[1];

// ------------------------------------------------------------
// depth 1
// g5:Z1, g1:Z2, g3:X3, g4:X4
// ------------------------------------------------------------
cz syn[2], data[0];
cz syn[0], data[1];
cx syn[1], data[2];
cx syn[3], data[3];

// ------------------------------------------------------------
// depth 2
// a = first g1--f1 coupling; e = first g5--f2 coupling
// ------------------------------------------------------------
cx syn[0], flag[0];
cx syn[2], flag[1];

// ------------------------------------------------------------
// depth 3
// g5:Z2, g1:Z3, b = first g3--f1, f = first g4--f2
// ------------------------------------------------------------
cz syn[2], data[1];
cz syn[0], data[2];
cx syn[1], flag[0];
cx syn[3], flag[1];

// ------------------------------------------------------------
// depth 4
// g4:Z1, g5:X3, g1:X4, g3:Z5
// ------------------------------------------------------------
cz syn[3], data[0];
cx syn[2], data[2];
cx syn[0], data[3];
cz syn[1], data[4];

// ------------------------------------------------------------
// depth 5
// g3:X1, g4:X2, c = second g1--f1, g = second g5--f2
// ------------------------------------------------------------
cx syn[1], data[0];
cx syn[3], data[1];
cx syn[0], flag[0];
cx syn[2], flag[1];

// ------------------------------------------------------------
// depth 6
// g1:X1, g5:X5, d = second g3--f1, h = second g4--f2
// ------------------------------------------------------------
cx syn[0], data[0];
cx syn[2], data[4];
cx syn[1], flag[0];
cx syn[3], flag[1];

// ------------------------------------------------------------
// depth 7
// g3:Z4, g4:Z5
// ------------------------------------------------------------
cz syn[1], data[3];
cz syn[3], data[4];

// Measure syndrome ancillas in X basis.
h syn[0];
m[0] = measure syn[0];  // m1
h syn[2];
m[1] = measure syn[2];  // m5
h syn[1];
m[2] = measure syn[1];  // m3
h syn[3];
m[3] = measure syn[3];  // m4

// Measure flags in Z basis.
f[0] = measure flag[0];
f[1] = measure flag[1];
