OPENQASM 3.0;
include "stdgates.inc";

// Parallel unflagged syndrome extraction for the Du et al. [[17,1,5]] code.
// Same four-generator partitions and data-CNOT order as Figures 5 and 6,
// with all flag couplings removed. Equation (3) numbering is used throughout.

qubit[17] data;
qubit[4] syn;
bit[16] m;

// Figure 5 partition, Z type: syn[0..3] = g8, g1, g3, g2.
reset syn[0]; reset syn[1]; reset syn[2]; reset syn[3];
cx data[2],syn[0]; cx data[8],syn[2];
cx data[5],syn[3]; cx data[2],syn[1];
cx data[3],syn[0]; cx data[9],syn[2];
cx data[2],syn[3]; cx data[3],syn[1];
cx data[5],syn[2]; cx data[0],syn[3];
cx data[14],syn[0]; cx data[1],syn[1];
cx data[4],syn[2]; cx data[13],syn[0];
cx data[4],syn[3]; cx data[0],syn[1];
cx data[10],syn[0]; cx data[6],syn[0];
cx data[9],syn[0]; cx data[5],syn[0];
m[7]=measure syn[0]; m[0]=measure syn[1];
m[2]=measure syn[2]; m[1]=measure syn[3];
barrier data,syn;

// Figure 6 partition, Z type: syn[0..3] = g4, g6, g7, g5.
reset syn[0]; reset syn[1]; reset syn[2]; reset syn[3];
cx data[10],syn[0]; cx data[7],syn[1];
cx data[15],syn[2]; cx data[12],syn[3];
cx data[11],syn[0]; cx data[8],syn[3];
cx data[14],syn[0]; cx data[6],syn[1];
cx data[13],syn[3]; cx data[11],syn[2];
cx data[11],syn[1]; cx data[15],syn[0];
cx data[16],syn[2]; cx data[9],syn[3];
cx data[10],syn[1]; cx data[7],syn[2];
m[3]=measure syn[0]; m[5]=measure syn[1];
m[6]=measure syn[2]; m[4]=measure syn[3];
barrier data,syn;

// Figure 5 partition, X dual: syn[0..3] = g16, g9, g11, g10.
reset syn[0]; reset syn[1]; reset syn[2]; reset syn[3];
h syn[0]; h syn[1]; h syn[2]; h syn[3];
cx syn[0],data[2]; cx syn[2],data[8];
cx syn[3],data[5]; cx syn[1],data[2];
cx syn[0],data[3]; cx syn[2],data[9];
cx syn[3],data[2]; cx syn[1],data[3];
cx syn[2],data[5]; cx syn[3],data[0];
cx syn[0],data[14]; cx syn[1],data[1];
cx syn[2],data[4]; cx syn[0],data[13];
cx syn[3],data[4]; cx syn[1],data[0];
cx syn[0],data[10]; cx syn[0],data[6];
cx syn[0],data[9]; cx syn[0],data[5];
h syn[0]; h syn[1]; h syn[2]; h syn[3];
m[15]=measure syn[0]; m[8]=measure syn[1];
m[10]=measure syn[2]; m[9]=measure syn[3];
barrier data,syn;

// Figure 6 partition, X dual: syn[0..3] = g12, g14, g15, g13.
reset syn[0]; reset syn[1]; reset syn[2]; reset syn[3];
h syn[0]; h syn[1]; h syn[2]; h syn[3];
cx syn[0],data[10]; cx syn[1],data[7];
cx syn[2],data[15]; cx syn[3],data[12];
cx syn[0],data[11]; cx syn[3],data[8];
cx syn[0],data[14]; cx syn[1],data[6];
cx syn[3],data[13]; cx syn[2],data[11];
cx syn[1],data[11]; cx syn[0],data[15];
cx syn[2],data[16]; cx syn[3],data[9];
cx syn[1],data[10]; cx syn[2],data[7];
h syn[0]; h syn[1]; h syn[2]; h syn[3];
m[11]=measure syn[0]; m[13]=measure syn[1];
m[14]=measure syn[2]; m[12]=measure syn[3];
