OPENQASM 3.0;
include "stdgates.inc";
// X-type dual of Du et al. Figure 5: C(g16,g9,g11,g10).
qubit[17] data; qubit[4] syn; qubit[3] flag;
bit[4] m; bit[3] f;
reset syn; h syn[0]; h syn[1]; h syn[2]; h syn[3]; reset flag;
cx syn[0],data[2]; cx syn[2],data[8]; cx syn[3],data[5]; cx syn[1],data[2];
cx syn[1],flag[1]; cx syn[0],flag[0]; cx syn[3],flag[2];
cx syn[0],data[3]; cx syn[2],flag[0]; cx syn[2],data[9];
cx syn[3],data[2]; cx syn[1],data[3]; cx syn[1],flag[0];
cx syn[2],data[5]; cx syn[3],data[0]; cx syn[0],data[14];
cx syn[0],flag[1]; cx syn[3],flag[2]; cx syn[1],data[1];
cx syn[2],flag[0]; cx syn[2],data[4]; cx syn[0],data[13];
cx syn[0],flag[0]; cx syn[3],data[4]; cx syn[0],flag[2];
cx syn[1],data[0]; cx syn[0],data[10]; cx syn[0],data[6];
cx syn[1],flag[0]; cx syn[0],data[9]; cx syn[0],flag[2];
cx syn[0],flag[0]; cx syn[0],flag[1]; cx syn[0],data[5];
h syn[0]; h syn[1]; h syn[2]; h syn[3]; m=measure syn; f=measure flag;
