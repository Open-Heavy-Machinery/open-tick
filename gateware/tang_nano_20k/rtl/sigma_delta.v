// SPDX-License-Identifier: Apache-2.0
// First-order sigma-delta DAC: the output's mean is value / 65536 of full
// scale. 0 gives a constant 0; 65535 is high 65535 cycles in 65536. An RC
// filter (or the Sallen-Key of the wiring proposal) on the pin makes it a
// voltage.
`default_nettype none
module sigma_delta (
    input  wire        clk,
    input  wire        rst,
    input  wire [15:0] value,
    output reg         out = 1'b0
);
    reg [16:0] acc = 17'd0;
    always @(posedge clk) begin
        if (rst) begin
            acc <= 17'd0;
            out <= 1'b0;
        end else begin
            acc <= {1'b0, acc[15:0]} + {1'b0, value};
            out <= acc[16];
        end
    end
endmodule
`default_nettype wire
