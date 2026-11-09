OPENQASM 3.0;
include "stdgates.inc";
// Du et al. Figure 5: C(g8,g1,g3,g2).
qubit[17] data; qubit[4] syn; qubit[3] flag;
bit[4] m; bit[3] f;
reset syn; reset flag; h flag[0]; h flag[1]; h flag[2];
cx data[2],syn[0]; cx data[8],syn[2]; cx data[5],syn[3]; cx data[2],syn[1];
cx flag[1],syn[1]; cx flag[0],syn[0]; cx flag[2],syn[3];
cx data[3],syn[0]; cx flag[0],syn[2]; cx data[9],syn[2];
cx data[2],syn[3]; cx data[3],syn[1]; cx flag[0],syn[1];
cx data[5],syn[2]; cx data[0],syn[3]; cx data[14],syn[0];
cx flag[1],syn[0]; cx flag[2],syn[3]; cx data[1],syn[1];
cx flag[0],syn[2]; cx data[4],syn[2]; cx data[13],syn[0];
cx flag[0],syn[0]; cx data[4],syn[3]; cx flag[2],syn[0];
cx data[0],syn[1]; cx data[10],syn[0]; cx data[6],syn[0];
cx flag[0],syn[1]; cx data[9],syn[0]; cx flag[2],syn[0];
cx flag[0],syn[0]; cx flag[1],syn[0]; cx data[5],syn[0];
m=measure syn; h flag[0]; h flag[1]; h flag[2]; f=measure flag;
