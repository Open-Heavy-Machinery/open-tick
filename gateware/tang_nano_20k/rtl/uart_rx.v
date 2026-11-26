// SPDX-License-Identifier: Apache-2.0
// 8N1 UART receiver. The line is synchronised with two flip-flops, the start
// bit is checked at its middle, and every data bit and the stop bit are
// sampled at their middle. A byte with a bad stop bit is dropped.
`default_nettype none
module uart_rx #(
    parameter integer CLKS_PER_BIT = 9 // 27 MHz / 3 Mbaud
) (
    input  wire       clk,
    input  wire       rst,
    input  wire       rx,
    output reg  [7:0] data,
    output reg        valid       // one cycle, with data
);
    localparam [1:0] IDLE = 2'd0, START = 2'd1, DATA = 2'd2, STOP = 2'd3;
    reg       rx_s1 = 1'b1, rx_s2 = 1'b1;
    reg [1:0] st = IDLE;
    reg [7:0] cnt = 8'd0;
    reg [2:0] bit_i = 3'd0;
    reg [7:0] sh = 8'd0;

    always @(posedge clk) begin
        rx_s1 <= rx;
        rx_s2 <= rx_s1;
    end

    always @(posedge clk) begin
        valid <= 1'b0;
        if (rst) begin
            st <= IDLE;
        end else begin
            case (st)
            IDLE:
                if (!rx_s2) begin
                    st  <= START;
                    cnt <= 8'd0;
                end
            START:
                if (cnt == (CLKS_PER_BIT - 1) / 2) begin
                    cnt   <= 8'd0;
                    bit_i <= 3'd0;
                    st    <= rx_s2 ? IDLE : DATA; // a glitch, not a start bit
                end else begin
                    cnt <= cnt + 8'd1;
                end
            DATA:
                if (cnt == CLKS_PER_BIT - 1) begin
                    cnt <= 8'd0;
                    sh  <= {rx_s2, sh[7:1]};
                    if (bit_i == 3'd7) st <= STOP;
                    bit_i <= bit_i + 3'd1;
                end else begin
                    cnt <= cnt + 8'd1;
                end
            default: // STOP
                if (cnt == CLKS_PER_BIT - 1) begin
                    st <= IDLE;
                    if (rx_s2) begin
                        data  <= sh;
                        valid <= 1'b1;
                    end
                end else begin
                    cnt <= cnt + 8'd1;
                end
            endcase
        end
    end
endmodule
`default_nettype wire
