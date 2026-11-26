// SPDX-License-Identifier: Apache-2.0
// open-tick's Tang Nano 20K bridge, protocol v1 (docs/bridge-protocol.md).
//
//   host --UART 3 Mbaud (BL616)--> frame_rx --> bridge_core --> uart_tx --> host
//                                                    |
//                         rotary (crank, cam) <------+------> sigma_delta x4 (analog)
//                         capture x6 (injectors) ----+
//
// One clock: the board's 27 MHz crystal. It is the run's clock (§1).
`default_nettype none
module top (
    input  wire       clk,          // 27 MHz
    input  wire       uart_rx,      // from the BL616
    output wire       uart_tx,      // to the BL616
    output wire       crank,
    output wire       cam,
    output wire [3:0] analog,
    input  wire [5:0] inj,
    output wire [5:0] led           // on-board LEDs, low = on
);
    // Power-on reset: 16 cycles.
    reg [4:0] por = 5'd0;
    wire rst = !por[4];
    always @(posedge clk) if (!por[4]) por <= por + 5'd1;

    wire [7:0]  rx_byte;
    wire        rx_valid;
    uart_rx #(.CLKS_PER_BIT(9)) u_rx (.clk(clk), .rst(rst), .rx(uart_rx), .data(rx_byte), .valid(rx_valid));

    wire        accept, msg_valid, msg_crc_ok;
    wire [7:0]  msg_type;
    wire [15:0] msg_seq, msg_len;
    wire [63:0] msg_t2, tick;
    wire [8:0]  raddr;
    wire [7:0]  rdata;
    frame_rx u_frame (
        .clk(clk), .rst(rst), .accept(accept), .byte_valid(rx_valid), .byte_data(rx_byte), .tick(tick),
        .msg_valid(msg_valid), .msg_crc_ok(msg_crc_ok), .msg_type(msg_type), .msg_seq(msg_seq),
        .msg_len(msg_len), .msg_t2(msg_t2), .raddr(raddr), .rdata(rdata));

    wire [7:0]  tx_data;
    wire        tx_start, tx_busy;
    uart_tx #(.CLKS_PER_BIT(9)) u_tx (.clk(clk), .rst(rst), .data(tx_data), .start(tx_start), .tx(uart_tx), .busy(tx_busy));

    wire        run;
    wire [2:0]  state;
    wire [31:0] phase_inc, crank_offset, cam_start, cam_end;
    wire [63:0] analog_v;
    wire [7:0]  teeth, missing, cap_mask, cap_pol;
    wire [5:0]  cap_count;
    wire [71:0] cap_head;
    wire        cap_pop, cap_flush, cap_lost;
    bridge_core u_core (
        .clk(clk), .rst(rst),
        .accept(accept), .msg_valid(msg_valid), .msg_crc_ok(msg_crc_ok), .msg_type(msg_type),
        .msg_seq(msg_seq), .msg_len(msg_len), .msg_t2(msg_t2), .raddr(raddr), .rdata(rdata),
        .tx_data(tx_data), .tx_start(tx_start), .tx_busy(tx_busy),
        .tick(tick), .run(run), .state(state), .phase_inc(phase_inc), .analog(analog_v),
        .crank_teeth(teeth), .crank_missing(missing), .crank_offset(crank_offset),
        .cam_start(cam_start), .cam_end(cam_end), .capture_mask(cap_mask), .capture_polarity(cap_pol),
        .cap_count(cap_count), .cap_head(cap_head), .cap_pop(cap_pop), .cap_flush(cap_flush), .cap_lost(cap_lost));

    rotary u_rot (
        .clk(clk), .rst(rst), .run(run), .inc(phase_inc), .teeth(teeth), .missing(missing),
        .offset(crank_offset), .cam_start(cam_start), .cam_end(cam_end), .crank(crank), .cam(cam),
        .phase_hi());

    capture #(.N(6), .DEPTH_LOG2(5)) u_cap (
        .clk(clk), .rst(rst), .run(run), .flush(cap_flush), .in(inj), .mask(cap_mask), .polarity(cap_pol),
        .tick(tick), .count(cap_count), .head(cap_head), .pop(cap_pop), .lost(cap_lost));

    genvar g;
    generate
        for (g = 0; g < 4; g = g + 1) begin : g_analog
            sigma_delta u_sd (.clk(clk), .rst(rst), .value(analog_v[16*g +: 16]), .out(analog[g]));
        end
    endgenerate

    // LEDs (low = on): the state in binary on 0-2, the crank on 3, the cam on 4, any capture on 5.
    assign led = ~{cap_count != 6'd0, cam, crank, state};
endmodule
`default_nettype wire
