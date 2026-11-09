OPENQASM 3.0;
include "stdgates.inc";

// Liou & Lai, arXiv:2407.00607, Fig. 8.
// [[5,1,3]] fully-parallel [1,1,1,1]^T flagged full syndrome extraction.
//
// Independent generators measured in this circuit:
//   g1 = X Z Z X I
//   g2 = I X Z Z X
//   g3 = X I X Z Z
//   g4 = Z X I X Z
//
// data[0..4] = physical data qubits 1..5.
// syn[0..3]  = measurement qubits m1..m4, prepared in |+> and measured in X.
// flag[0..3] = flag qubits f1..f4, prepared in |0> and measured in Z.
//
// The two-qubit part has depth 6 and 24 two-qubit gates, matching Fig. 8.

qubit[5] data;
qubit[4] syn;
qubit[4] flag;
bit[4] m;
bit[4] f;

// Prepare measurement ancillas in |+>.
reset syn[0];
h syn[0];
reset syn[1];
h syn[1];
reset syn[2];
h syn[2];
reset syn[3];
h syn[3];

// Prepare flag ancillas in |0>.
reset flag[0];
reset flag[1];
reset flag[2];
reset flag[3];

// ------------------------------------------------------------
// Two-qubit depth 1: first X component of each generator.
// g1:X1, g2:X2, g3:X3, g4:X4
// ------------------------------------------------------------
cx syn[0], data[0];
cx syn[1], data[1];
cx syn[2], data[2];
cx syn[3], data[3];

// ------------------------------------------------------------
// Two-qubit depth 2: first flag coupling.
// ------------------------------------------------------------
cx syn[0], flag[0];
cx syn[1], flag[1];
cx syn[2], flag[2];
cx syn[3], flag[3];

// ------------------------------------------------------------
// Two-qubit depth 3: first Z component of each generator.
// g1:Z2, g2:Z3, g3:Z4, g4:Z5
// ------------------------------------------------------------
cz syn[0], data[1];
cz syn[1], data[2];
cz syn[2], data[3];
cz syn[3], data[4];

// ------------------------------------------------------------
// Two-qubit depth 4: second Z component of each generator.
// g1:Z3, g2:Z4, g3:Z5, g4:Z1
// ------------------------------------------------------------
cz syn[0], data[2];
cz syn[1], data[3];
cz syn[2], data[4];
cz syn[3], data[0];

// ------------------------------------------------------------
// Two-qubit depth 5: second flag coupling.
// ------------------------------------------------------------
cx syn[0], flag[0];
cx syn[1], flag[1];
cx syn[2], flag[2];
cx syn[3], flag[3];

// ------------------------------------------------------------
// Two-qubit depth 6: second X component of each generator.
// g1:X4, g2:X5, g3:X1, g4:X2
// ------------------------------------------------------------
cx syn[0], data[3];
cx syn[1], data[4];
cx syn[2], data[0];
cx syn[3], data[1];

// Measure syndrome ancillas in the X basis.
h syn[0];
m[0] = measure syn[0];
h syn[1];
m[1] = measure syn[1];
h syn[2];
m[2] = measure syn[2];
h syn[3];
m[3] = measure syn[3];

// Measure flags in the Z basis.
f[0] = measure flag[0];
f[1] = measure flag[1];
f[2] = measure flag[2];
f[3] = measure flag[3];
