// SPDX-License-Identifier: Apache-2.0
// CRC-16/IBM-3740 (CCITT-FALSE): polynomial 0x1021, MSB first, one byte.
// The same CRC as otk_bridge_crc16() in open-tick's core/src/bridge.c.
function [15:0] crc16_byte;
    input [15:0] crc;
    input [7:0]  b;
    integer i;
    reg [15:0] c;
    begin
        c = crc ^ {b, 8'h00};
        for (i = 0; i < 8; i = i + 1)
            c = c[15] ? ({c[14:0], 1'b0} ^ 16'h1021) : {c[14:0], 1'b0};
        crc16_byte = c;
    end
endfunction
