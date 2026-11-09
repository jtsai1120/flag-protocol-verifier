OPENQASM 3.0;
include "stdgates.inc";

// [[5,1,3]] complete RAW (unflagged) syndrome extraction for the same
// independent generator set used by Liou & Lai's fully-parallel [1,1,1,1]^T
// flagged scheme in Fig. 8 of arXiv:2407.00607.
//
//   g1 = X Z Z X I
//   g2 = I X Z Z X
//   g3 = X I X Z Z
//   g4 = Z X I X Z
//
// This file keeps the same cyclic/interleaved data-gate ordering as Fig. 8,
// but removes the two flag-coupling layers. Hence the two-qubit data-SE part
// has depth 4 and uses 16 two-qubit gates.
//
// data[0..4] = physical data qubits 1..5.
// syn[0..3]  = measurement qubits m1..m4, prepared in |+> and measured in X.

qubit[5] data;
qubit[4] syn;
bit[4] m;

// Prepare measurement ancillas in |+>.
reset syn[0];
h syn[0];
reset syn[1];
h syn[1];
reset syn[2];
h syn[2];
reset syn[3];
h syn[3];

// ------------------------------------------------------------
// Two-qubit depth 1: X layer.
// g1:X1, g2:X2, g3:X3, g4:X4
// ------------------------------------------------------------
cx syn[0], data[0];
cx syn[1], data[1];
cx syn[2], data[2];
cx syn[3], data[3];

// ------------------------------------------------------------
// Two-qubit depth 2: Z layer.
// g1:Z2, g2:Z3, g3:Z4, g4:Z5
// ------------------------------------------------------------
cz syn[0], data[1];
cz syn[1], data[2];
cz syn[2], data[3];
cz syn[3], data[4];

// ------------------------------------------------------------
// Two-qubit depth 3: Z layer.
// g1:Z3, g2:Z4, g3:Z5, g4:Z1
// ------------------------------------------------------------
cz syn[0], data[2];
cz syn[1], data[3];
cz syn[2], data[4];
cz syn[3], data[0];

// ------------------------------------------------------------
// Two-qubit depth 4: X layer.
// g1:X4, g2:X5, g3:X1, g4:X2
// ------------------------------------------------------------
cx syn[0], data[3];
cx syn[1], data[4];
cx syn[2], data[0];
cx syn[3], data[1];

// Measure complete syndrome in the X basis.
h syn[0];
m[0] = measure syn[0];
h syn[1];
m[1] = measure syn[1];
h syn[2];
m[2] = measure syn[2];
h syn[3];
m[3] = measure syn[3];
