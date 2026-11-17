OPENQASM 3.0;
include "stdgates.inc";

// [[5,1,3]] [2,2]^T fix_2: the corrected [2,2]^T circuit from
// fmlab-iis/flag-analysis ("[[5,1,3]]_[2,2]_T_fix_2"), with shared flags.
// It measures the declared generators g1..g4 (the original measures the
// fifth cyclic generator ZZXIX instead of IXZZX) and is layered so that
// every pair of ancillas meets anticommuting Paulis in the same order:
// first CX of every ancilla, flag CNOTs, both CZs, flag CNOTs, last CX.
//
//   syn[0]: X Z Z X I   (X q0, Z q1, Z q2, X q3)
//   syn[1]: I X Z Z X   (X q1, Z q3, Z q2, X q4)
//   syn[2]: X I X Z Z   (X q2, Z q3, Z q4, X q0)
//   syn[3]: Z X I X Z   (X q3, Z q0, Z q4, X q1)
//   flag[1] is shared by syn[0] and syn[1], flag[0] by syn[2] and syn[3].

qubit[5] data;
qubit[4] syn;
qubit[2] flag;
bit[4] m;
bit[2] f;

// Measurement ancillas in |+>.
reset syn[0];
h syn[0];
reset syn[1];
h syn[1];
reset syn[2];
h syn[2];
reset syn[3];
h syn[3];

// Flags in |0>.
reset flag[0];
reset flag[1];

// First data gate of every ancilla.
cx syn[0], data[0];
cx syn[1], data[1];
cx syn[2], data[2];
cx syn[3], data[3];

// Open the flags.
cx syn[0], flag[1];
cx syn[2], flag[0];
cx syn[1], flag[1];
cx syn[3], flag[0];

// Middle CZs.
cz data[1], syn[0];
cz data[3], syn[2];
cz data[2], syn[0];
cz data[3], syn[1];
cz data[4], syn[2];
cz data[0], syn[3];
cz data[2], syn[1];
cz data[4], syn[3];

// Close the flags.
cx syn[0], flag[1];
cx syn[2], flag[0];
cx syn[1], flag[1];
cx syn[3], flag[0];

// Last data gate of every ancilla.
cx syn[0], data[3];
cx syn[1], data[4];
cx syn[2], data[0];
cx syn[3], data[1];

// Measure the ancillas in the X basis.
h syn[0];
m[0] = measure syn[0];
h syn[1];
m[1] = measure syn[1];
h syn[2];
m[2] = measure syn[2];
h syn[3];
m[3] = measure syn[3];

// Measure the flags in the Z basis.
f[0] = measure flag[0];
f[1] = measure flag[1];
