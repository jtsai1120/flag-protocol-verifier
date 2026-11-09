OPENQASM 3.0;
include "stdgates.inc";
// Du et al. Figure 6: C(g4,g6,g7,g5).
qubit[17] data; qubit[4] syn; qubit[2] flag;
bit[4] m; bit[2] f;
reset syn; reset flag; h flag[0]; h flag[1];
cx data[10],syn[0]; cx data[7],syn[1]; cx data[15],syn[2]; cx data[12],syn[3];
cx flag[0],syn[0]; cx flag[1],syn[3]; cx data[11],syn[0];
cx flag[0],syn[1]; cx data[8],syn[3]; cx data[14],syn[0];
cx flag[0],syn[2]; cx data[6],syn[1]; cx data[13],syn[3];
cx flag[0],syn[0]; cx data[11],syn[2]; cx flag[1],syn[2];
cx data[11],syn[1]; cx data[15],syn[0]; cx data[16],syn[2];
cx data[9],syn[3]; cx flag[0],syn[3]; cx data[10],syn[1];
cx flag[0],syn[2]; cx data[7],syn[2];
m=measure syn; h flag[0]; h flag[1]; f=measure flag;
