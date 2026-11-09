OPENQASM 3.0;
include "stdgates.inc";
// X-type dual of Du et al. Figure 6: C(g12,g14,g15,g13).
qubit[17] data; qubit[4] syn; qubit[2] flag;
bit[4] m; bit[2] f;
reset syn; h syn[0]; h syn[1]; h syn[2]; h syn[3]; reset flag;
cx syn[0],data[10]; cx syn[1],data[7]; cx syn[2],data[15]; cx syn[3],data[12];
cx syn[0],flag[0]; cx syn[3],flag[1]; cx syn[0],data[11];
cx syn[1],flag[0]; cx syn[3],data[8]; cx syn[0],data[14];
cx syn[2],flag[0]; cx syn[1],data[6]; cx syn[3],data[13];
cx syn[0],flag[0]; cx syn[2],data[11]; cx syn[2],flag[1];
cx syn[1],data[11]; cx syn[0],data[15]; cx syn[2],data[16];
cx syn[3],data[9]; cx syn[3],flag[0]; cx syn[1],data[10];
cx syn[2],flag[0]; cx syn[2],data[7];
h syn[0]; h syn[1]; h syn[2]; h syn[3]; m=measure syn; f=measure flag;
