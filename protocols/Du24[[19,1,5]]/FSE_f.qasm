OPENQASM 3.0;
include "stdgates.inc";

// One atomic flagged round for the [[19,1,5]] code in Du et al.
// Figures 8--10 are executed here as six three-generator blocks: the
// three Z blocks followed by their X-type duals.  The returned syndrome
// follows the paper's Equation (4) generator order g1,...,g18.
qubit[19] data; qubit[3] syn; qubit[2] flag;
bit[18] m; bit[12] f;

// Figure 8 Z block.  Ancilla rows are g7, g1, g2.
reset syn; reset flag; h flag[0]; h flag[1];
cx data[1],syn[0]; cx flag[1],syn[0];
cx data[8],syn[0]; cx flag[0],syn[0]; cx data[7],syn[0];
cx data[1],syn[1]; cx data[4],syn[2];
cx flag[1],syn[1]; cx flag[0],syn[2];
cx data[5],syn[0]; cx data[3],syn[1]; cx data[0],syn[2];
cx data[2],syn[1]; cx data[2],syn[2];
cx flag[1],syn[1]; cx flag[0],syn[2];
cx data[0],syn[1]; cx flag[1],syn[0];
cx data[6],syn[2]; cx data[4],syn[0];
cx flag[0],syn[0]; cx data[0],syn[0];
m[6]=measure syn[0]; m[0]=measure syn[1]; m[1]=measure syn[2];
h flag[0]; h flag[1];
f[0]=measure flag[0]; f[1]=measure flag[1];
barrier data,syn,flag;

// Figure 9 Z block.  Ancilla rows are g8, g5, g4.
reset syn; reset flag; h flag[0]; h flag[1];
cx data[16],syn[0]; cx flag[1],syn[0];
cx data[10],syn[0]; cx flag[0],syn[0]; cx data[9],syn[0];
cx data[16],syn[1]; cx data[8],syn[2];
cx flag[1],syn[1]; cx flag[0],syn[2];
cx data[7],syn[0]; cx data[17],syn[1]; cx data[15],syn[2];
cx data[18],syn[1]; cx data[18],syn[2];
cx flag[1],syn[1]; cx flag[0],syn[2];
cx data[15],syn[1]; cx flag[1],syn[0];
cx data[5],syn[2]; cx data[8],syn[0];
cx flag[0],syn[0]; cx data[15],syn[0];
m[7]=measure syn[0]; m[4]=measure syn[1]; m[3]=measure syn[2];
h flag[0]; h flag[1];
f[2]=measure flag[0]; f[3]=measure flag[1];
barrier data,syn,flag;

// Figure 10 Z block.  Ancilla rows are g9, g3, g6.
reset syn; reset flag; h flag[0]; h flag[1];
cx data[12],syn[0]; cx flag[1],syn[0];
cx data[7],syn[0]; cx flag[0],syn[0]; cx data[4],syn[0];
cx data[12],syn[1]; cx data[10],syn[2];
cx flag[1],syn[1]; cx flag[0],syn[2];
cx data[6],syn[0]; cx data[13],syn[1]; cx data[11],syn[2];
cx data[14],syn[1]; cx data[14],syn[2];
cx flag[1],syn[1]; cx flag[0],syn[2];
cx data[11],syn[1]; cx flag[1],syn[0];
cx data[9],syn[2]; cx data[10],syn[0];
cx flag[0],syn[0]; cx data[11],syn[0];
m[8]=measure syn[0]; m[2]=measure syn[1]; m[5]=measure syn[2];
h flag[0]; h flag[1];
f[4]=measure flag[0]; f[5]=measure flag[1];
barrier data,syn,flag;

// Figure 8 X dual.  Ancilla rows are g16, g10, g11.
reset syn; h syn[0]; h syn[1]; h syn[2]; reset flag;
cx syn[0],data[1]; cx syn[0],flag[1];
cx syn[0],data[8]; cx syn[0],flag[0]; cx syn[0],data[7];
cx syn[1],data[1]; cx syn[2],data[4];
cx syn[1],flag[1]; cx syn[2],flag[0];
cx syn[0],data[5]; cx syn[1],data[3]; cx syn[2],data[0];
cx syn[1],data[2]; cx syn[2],data[2];
cx syn[1],flag[1]; cx syn[2],flag[0];
cx syn[1],data[0]; cx syn[0],flag[1];
cx syn[2],data[6]; cx syn[0],data[4];
cx syn[0],flag[0]; cx syn[0],data[0];
h syn[0]; h syn[1]; h syn[2];
m[15]=measure syn[0]; m[9]=measure syn[1]; m[10]=measure syn[2];
f[6]=measure flag[0]; f[7]=measure flag[1];
barrier data,syn,flag;

// Figure 9 X dual.  Ancilla rows are g17, g14, g13.
reset syn; h syn[0]; h syn[1]; h syn[2]; reset flag;
cx syn[0],data[16]; cx syn[0],flag[1];
cx syn[0],data[10]; cx syn[0],flag[0]; cx syn[0],data[9];
cx syn[1],data[16]; cx syn[2],data[8];
cx syn[1],flag[1]; cx syn[2],flag[0];
cx syn[0],data[7]; cx syn[1],data[17]; cx syn[2],data[15];
cx syn[1],data[18]; cx syn[2],data[18];
cx syn[1],flag[1]; cx syn[2],flag[0];
cx syn[1],data[15]; cx syn[0],flag[1];
cx syn[2],data[5]; cx syn[0],data[8];
cx syn[0],flag[0]; cx syn[0],data[15];
h syn[0]; h syn[1]; h syn[2];
m[16]=measure syn[0]; m[13]=measure syn[1]; m[12]=measure syn[2];
f[8]=measure flag[0]; f[9]=measure flag[1];
barrier data,syn,flag;

// Figure 10 X dual.  Ancilla rows are g18, g12, g15.
reset syn; h syn[0]; h syn[1]; h syn[2]; reset flag;
cx syn[0],data[12]; cx syn[0],flag[1];
cx syn[0],data[7]; cx syn[0],flag[0]; cx syn[0],data[4];
cx syn[1],data[12]; cx syn[2],data[10];
cx syn[1],flag[1]; cx syn[2],flag[0];
cx syn[0],data[6]; cx syn[1],data[13]; cx syn[2],data[11];
cx syn[1],data[14]; cx syn[2],data[14];
cx syn[1],flag[1]; cx syn[2],flag[0];
cx syn[1],data[11]; cx syn[0],flag[1];
cx syn[2],data[9]; cx syn[0],data[10];
cx syn[0],flag[0]; cx syn[0],data[11];
h syn[0]; h syn[1]; h syn[2];
m[17]=measure syn[0]; m[11]=measure syn[1]; m[14]=measure syn[2];
f[10]=measure flag[0]; f[11]=measure flag[1];
