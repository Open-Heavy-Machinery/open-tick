// SPDX-License-Identifier: Apache-2.0
// Unit testbench for the bridge's blocks: UART, CRC, sigma-delta, rotary
// pattern, capture. Prints one line per check that fails and a summary
// "tb_units: N checks, M failed"; sim/run.sh fails on M > 0.
`timescale 1ns / 1ps
`default_nettype none
module tb_units;
`include "crc16.vh"
    reg clk = 1'b0;
    always #18.5 clk = ~clk; // 27 MHz
    reg rst = 1'b1;
    reg [63:0] tick = 64'd0;
    always @(posedge clk) tick <= tick + 64'd1;

    integer checks = 0, fails = 0;
    task check(input cond, input [8*64-1:0] what);
        begin
            checks = checks + 1;
            if (!cond) begin
                fails = fails + 1;
                $display("FAIL %0s (t=%0d)", what, tick);
            end
        end
    endtask

    // ---------------------------------------------------------------- UART loopback
    reg  [7:0] tx_d = 8'd0;
    reg        tx_go = 1'b0;
    wire       line, tx_busy, rx_v;
    wire [7:0] rx_d;
    uart_tx #(.CLKS_PER_BIT(9)) u_tx (.clk(clk), .rst(rst), .data(tx_d), .start(tx_go), .tx(line), .busy(tx_busy));
    uart_rx #(.CLKS_PER_BIT(9)) u_rx (.clk(clk), .rst(rst), .rx(line), .data(rx_d), .valid(rx_v));
    reg [7:0] got [0:299];
    integer n_got = 0;
    always @(posedge clk) if (rx_v) begin got[n_got] <= rx_d; n_got <= n_got + 1; end

    // A second receiver, fed by a sender whose baud rate is off by a few per
    // cent (the BL616 and the FPGA have different clocks).
    reg        skew_line = 1'b1;
    wire       skew_v;
    wire [7:0] skew_d;
    uart_rx #(.CLKS_PER_BIT(9)) u_rx_skew (.clk(clk), .rst(rst), .rx(skew_line), .data(skew_d), .valid(skew_v));
    reg [7:0] skew_got [0:63];
    integer n_skew = 0;
    always @(posedge clk) if (skew_v) begin skew_got[n_skew] <= skew_d; n_skew <= n_skew + 1; end
    task send_skewed(input [7:0] v, input real bit_ns);
        integer q;
        begin
            skew_line = 1'b0;
            #(bit_ns);
            for (q = 0; q < 8; q = q + 1) begin
                skew_line = v[q];
                #(bit_ns);
            end
            skew_line = 1'b1;
            #(bit_ns);
        end
    endtask

    // ---------------------------------------------------------------- sigma-delta
    reg  [15:0] sd_v = 16'd0;
    reg         sd_rst = 1'b1;
    wire        sd_out;
    sigma_delta u_sd (.clk(clk), .rst(sd_rst), .value(sd_v), .out(sd_out));

    // ---------------------------------------------------------------- rotary
    reg         r_run = 1'b0;
    reg  [31:0] r_inc = 32'd0;
    wire        crank, cam;
    wire [31:0] phase_hi;
    rotary u_rot (.clk(clk), .rst(rst), .run(r_run), .inc(r_inc), .teeth(8'd60), .missing(8'd2),
                  .offset(32'd0), .cam_start(32'd0), .cam_end(32'h80000000),
                  .crank(crank), .cam(cam), .phase_hi(phase_hi));
    reg crank_d = 1'b0, cam_d = 1'b0;
    integer crank_rises = 0, cam_rises = 0;
    reg [63:0] last_rise = 64'd0, min_gap = 64'hFFFFFFFFFFFFFFFF, max_gap = 64'd0;
    always @(posedge clk) begin
        crank_d <= crank;
        cam_d <= cam;
        if (crank && !crank_d) begin
            crank_rises <= crank_rises + 1;
            if (last_rise != 0) begin
                if (tick - last_rise < min_gap) min_gap <= tick - last_rise;
                if (tick - last_rise > max_gap) max_gap <= tick - last_rise;
            end
            last_rise <= tick;
        end
        if (cam && !cam_d) cam_rises <= cam_rises + 1;
    end

    // ---------------------------------------------------------------- capture
    reg  [5:0]  inj = 6'b000010; // channel 1 is active low: idle high
    reg         c_run = 1'b0, c_flush = 1'b0, c_pop = 1'b0;
    wire [5:0]  c_count;
    wire [71:0] c_head;
    wire        c_lost;
    capture #(.N(6), .DEPTH_LOG2(5)) u_cap (.clk(clk), .rst(rst), .run(c_run), .flush(c_flush), .in(inj),
        .mask(8'b00000111), .polarity(8'b11111101), .tick(tick), .count(c_count), .head(c_head),
        .pop(c_pop), .lost(c_lost));
    integer n_lost = 0;
    always @(posedge clk) if (c_lost) n_lost <= n_lost + 1;

    integer i, ones, k;
    reg [15:0] c;
    reg [63:0] t0, t_a, t_b;
    reg [31:0] d0;
    initial begin
        repeat (4) @(posedge clk);
        rst = 1'b0;

        // ---- CRC: the IBM-3740 check value
        c = 16'hFFFF;
        c = crc16_byte(c, "1"); c = crc16_byte(c, "2"); c = crc16_byte(c, "3");
        c = crc16_byte(c, "4"); c = crc16_byte(c, "5"); c = crc16_byte(c, "6");
        c = crc16_byte(c, "7"); c = crc16_byte(c, "8"); c = crc16_byte(c, "9");
        check(c == 16'h29B1, "crc16 check value");

        // ---- UART: 300 bytes back to back
        for (i = 0; i < 300; i = i + 1) begin
            @(posedge clk);
            while (tx_busy) @(posedge clk);
            tx_d <= (i * 37 + 11) & 8'hFF;
            tx_go <= 1'b1;
            @(posedge clk);
            tx_go <= 1'b0;
            @(posedge clk);
        end
        repeat (200) @(posedge clk);
        check(n_got == 300, "uart: 300 bytes received");
        for (i = 0; i < 300; i = i + 1)
            if (got[i] != ((i * 37 + 11) & 8'hFF)) check(1'b0, "uart: byte value");

        // ---- UART receiver against a sender 3 % slow, then 3 % fast (333.3 ns per bit at 3 Mbaud)
        for (i = 0; i < 32; i = i + 1) send_skewed((i * 53 + 7) & 8'hFF, 333.333 * 1.03);
        for (i = 32; i < 64; i = i + 1) send_skewed((i * 53 + 7) & 8'hFF, 333.333 * 0.97);
        repeat (100) @(posedge clk);
        check(n_skew == 64, "uart_rx: 64 bytes at +/-3 % baud");
        for (i = 0; i < 64; i = i + 1)
            if (skew_got[i] != ((i * 53 + 7) & 8'hFF)) check(1'b0, "uart_rx: byte value at +/-3 % baud");

        // ---- sigma-delta: exactly `value` ones in 65536 cycles
        for (k = 0; k < 5; k = k + 1) begin
            sd_v <= (k == 0) ? 16'd0 : (k == 1) ? 16'd1 : (k == 2) ? 16'd32768 : (k == 3) ? 16'd65535 : 16'd12345;
            sd_rst <= 1'b1;
            @(posedge clk);
            sd_rst <= 1'b0;
            // `out` is registered after the accumulator, and this block reads values
            // from before each edge: count the 65536 cycles two edges later.
            @(posedge clk);
            @(posedge clk);
            ones = 0;
            for (i = 0; i < 65536; i = i + 1) begin
                @(posedge clk);
                if (sd_out) ones = ones + 1;
            end
            if (ones != sd_v) $display("sigma-delta value %0d: %0d ones", sd_v, ones);
            check(ones == sd_v, "sigma-delta: density");
        end

        // ---- rotary: inc = 2^32 -> 720 degrees in 65536 ticks, 360 in 32768, a tooth every 546.13
        r_inc <= 32'h80000000; // 2^31: 720 degrees in 131072 ticks, a tooth every 1092.3
        r_run <= 1'b1;
        t0 = tick;
        crank_rises = 0; cam_rises = 0;
        last_rise = 0; min_gap = 64'hFFFFFFFFFFFFFFFF; max_gap = 0;
        repeat (131072 * 2) @(posedge clk);
        // two 720-degree cycles = four revolutions of 58 teeth
        check(crank_rises >= 231 && crank_rises <= 233, "rotary: 58 teeth per revolution");
        check(cam_rises == 2, "rotary: one cam pulse per 720 degrees");
        check(min_gap >= 1091 && min_gap <= 1094, "rotary: tooth pitch");
        check(max_gap >= 3275 && max_gap <= 3279, "rotary: the gap of two missing teeth is three pitches");
        // a speed change keeps the phase: no tooth shorter than the faster pitch
        min_gap = 64'hFFFFFFFFFFFFFFFF;
        r_inc <= 32'hFFFFFFFF; // twice the speed: a tooth every 546.1
        repeat (65536) @(posedge clk);
        check(min_gap >= 545, "rotary: phase-continuous across a speed change");
        // at rest the outputs hold
        r_run <= 1'b0;
        @(posedge clk);
        t_a = crank_rises;
        repeat (20000) @(posedge clk);
        check(crank_rises == t_a, "rotary: no edges at rest");

        // ---- capture
        c_run <= 1'b1;
        @(posedge clk);
        @(posedge clk);
        // channel 0 (active high): a pulse of 300 ticks
        @(posedge clk); #1 inj[0] = 1'b1; t_a = tick;
        repeat (300) @(posedge clk); #1 inj[0] = 1'b0;
        // channel 1 (active low): a pulse of 200 ticks
        repeat (50) @(posedge clk); #1 inj[1] = 1'b0; t_b = tick;
        repeat (200) @(posedge clk); #1 inj[1] = 1'b1;
        // channel 3 is masked: ignored
        repeat (50) @(posedge clk); #1 inj[3] = 1'b1;
        repeat (100) @(posedge clk); #1 inj[3] = 1'b0;
        // channels 0 and 2 end in the same cycle: both kept
        repeat (50) @(posedge clk); #1 inj[0] = 1'b1; inj[2] = 1'b1;
        repeat (120) @(posedge clk); #1 inj[0] = 1'b0; inj[2] = 1'b0;
        repeat (20) @(posedge clk);
        check(c_count == 6'd4, "capture: four captures, masked channel ignored");
        // first: channel 0, width 300; the start is a constant delay after the edge
        check(c_head[7:0] == 8'd0 && c_head[71:40] == 32'd300, "capture: channel 0 width");
        d0 = c_head[39:8] - t_a[31:0];
        $display("capture: SYNC_DELAY = %0d ticks", d0);
        @(posedge clk); c_pop <= 1'b1; @(posedge clk); c_pop <= 1'b0; repeat (3) @(posedge clk);
        check(c_head[7:0] == 8'd1 && c_head[71:40] == 32'd200, "capture: channel 1 (active low) width");
        check(c_head[39:8] - t_b[31:0] == d0, "capture: same delay on every channel");
        @(posedge clk); c_pop <= 1'b1; @(posedge clk); c_pop <= 1'b0; repeat (3) @(posedge clk);
        check(c_head[7:0] == 8'd0 && c_head[71:40] == 32'd120, "capture: simultaneous end, channel 0 first");
        @(posedge clk); c_pop <= 1'b1; @(posedge clk); c_pop <= 1'b0; repeat (3) @(posedge clk);
        check(c_head[7:0] == 8'd2 && c_head[71:40] == 32'd120, "capture: simultaneous end, channel 2 kept");
        @(posedge clk); c_pop <= 1'b1; @(posedge clk); c_pop <= 1'b0; repeat (3) @(posedge clk);
        check(c_count == 6'd0, "capture: empty after four pops");
        // 34 pulses into a 32-deep FIFO: 2 lost
        for (i = 0; i < 34; i = i + 1) begin
            repeat (10) @(posedge clk); #1 inj[0] = 1'b1;
            repeat (10) @(posedge clk); #1 inj[0] = 1'b0;
        end
        repeat (10) @(posedge clk);
        check(c_count == 6'd32 && n_lost == 2, "capture: overflow counted");
        @(posedge clk); c_flush <= 1'b1; @(posedge clk); c_flush <= 1'b0; repeat (2) @(posedge clk);
        check(c_count == 6'd0, "capture: flush");

        $display("tb_units: %0d checks, %0d failed", checks, fails);
        $finish;
    end
endmodule
`default_nettype wire
