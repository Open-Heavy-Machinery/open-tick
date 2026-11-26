// SPDX-License-Identifier: Apache-2.0
// 8N1 UART transmitter: start bit, eight data bits LSB first, one stop bit.
`default_nettype none
module uart_tx #(
    parameter integer CLKS_PER_BIT = 9
) (
    input  wire       clk,
    input  wire       rst,
    input  wire [7:0] data,
    input  wire       start,  // ignored while busy
    output reg        tx = 1'b1,
    output wire       busy
);
    reg [9:0] sh = 10'h3FF;
    reg [3:0] n = 4'd0;
    reg [7:0] cnt = 8'd0;
    reg       active = 1'b0;
    assign busy = active;

    always @(posedge clk) begin
        if (rst) begin
            tx     <= 1'b1;
            active <= 1'b0;
        end else if (!active) begin
            tx <= 1'b1;
            if (start) begin
                sh     <= {1'b1, data, 1'b0};
                active <= 1'b1;
                n      <= 4'd0;
                cnt    <= 8'd0;
            end
        end else begin
            tx <= sh[0];
            if (cnt == CLKS_PER_BIT - 1) begin
                cnt <= 8'd0;
                sh  <= {1'b1, sh[9:1]};
                if (n == 4'd9) active <= 1'b0;
                n <= n + 4'd1;
            end else begin
                cnt <= cnt + 8'd1;
            end
        end
    end
endmodule
`default_nettype wire
