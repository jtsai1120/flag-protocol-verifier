OPENQASM 3.0;
include "stdgates.inc";

// Parallel unflagged round using the same Figure 8--10 partitions and
// the same Equation (4) syndrome-bit order as FSE_f.qasm.
qubit[19] data; qubit[3] syn; bit[18] m;

// Figure 8 Z: ancilla rows g7, g1, g2.
reset syn;
cx data[1],syn[0]; cx data[8],syn[0]; cx data[7],syn[0];
cx data[1],syn[1]; cx data[4],syn[2]; cx data[5],syn[0];
cx data[3],syn[1]; cx data[0],syn[2]; cx data[2],syn[1];
cx data[2],syn[2]; cx data[0],syn[1]; cx data[6],syn[2];
cx data[4],syn[0]; cx data[0],syn[0];
m[6]=measure syn[0]; m[0]=measure syn[1]; m[1]=measure syn[2];
barrier data,syn;

// Figure 9 Z: ancilla rows g8, g5, g4.
reset syn;
cx data[16],syn[0]; cx data[10],syn[0]; cx data[9],syn[0];
cx data[16],syn[1]; cx data[8],syn[2]; cx data[7],syn[0];
cx data[17],syn[1]; cx data[15],syn[2]; cx data[18],syn[1];
cx data[18],syn[2]; cx data[15],syn[1]; cx data[5],syn[2];
cx data[8],syn[0]; cx data[15],syn[0];
m[7]=measure syn[0]; m[4]=measure syn[1]; m[3]=measure syn[2];
barrier data,syn;

// Figure 10 Z: ancilla rows g9, g3, g6.
reset syn;
cx data[12],syn[0]; cx data[7],syn[0]; cx data[4],syn[0];
cx data[12],syn[1]; cx data[10],syn[2]; cx data[6],syn[0];
cx data[13],syn[1]; cx data[11],syn[2]; cx data[14],syn[1];
cx data[14],syn[2]; cx data[11],syn[1]; cx data[9],syn[2];
cx data[10],syn[0]; cx data[11],syn[0];
m[8]=measure syn[0]; m[2]=measure syn[1]; m[5]=measure syn[2];
barrier data,syn;

// Figure 8 X dual: ancilla rows g16, g10, g11.
reset syn; h syn[0]; h syn[1]; h syn[2];
cx syn[0],data[1]; cx syn[0],data[8]; cx syn[0],data[7];
cx syn[1],data[1]; cx syn[2],data[4]; cx syn[0],data[5];
cx syn[1],data[3]; cx syn[2],data[0]; cx syn[1],data[2];
cx syn[2],data[2]; cx syn[1],data[0]; cx syn[2],data[6];
cx syn[0],data[4]; cx syn[0],data[0];
h syn[0]; h syn[1]; h syn[2];
m[15]=measure syn[0]; m[9]=measure syn[1]; m[10]=measure syn[2];
barrier data,syn;

// Figure 9 X dual: ancilla rows g17, g14, g13.
reset syn; h syn[0]; h syn[1]; h syn[2];
cx syn[0],data[16]; cx syn[0],data[10]; cx syn[0],data[9];
cx syn[1],data[16]; cx syn[2],data[8]; cx syn[0],data[7];
cx syn[1],data[17]; cx syn[2],data[15]; cx syn[1],data[18];
cx syn[2],data[18]; cx syn[1],data[15]; cx syn[2],data[5];
cx syn[0],data[8]; cx syn[0],data[15];
h syn[0]; h syn[1]; h syn[2];
m[16]=measure syn[0]; m[13]=measure syn[1]; m[12]=measure syn[2];
barrier data,syn;

// Figure 10 X dual: ancilla rows g18, g12, g15.
reset syn; h syn[0]; h syn[1]; h syn[2];
cx syn[0],data[12]; cx syn[0],data[7]; cx syn[0],data[4];
cx syn[1],data[12]; cx syn[2],data[10]; cx syn[0],data[6];
cx syn[1],data[13]; cx syn[2],data[11]; cx syn[1],data[14];
cx syn[2],data[14]; cx syn[1],data[11]; cx syn[2],data[9];
cx syn[0],data[10]; cx syn[0],data[11];
h syn[0]; h syn[1]; h syn[2];
m[17]=measure syn[0]; m[11]=measure syn[1]; m[14]=measure syn[2];
