// SPDX-License-Identifier: Apache-2.0
// System testbench: the whole bridge (top.v) driven over its UART by a script
// that open-tick's codec wrote (bridge_vectors.c gen), and logged so that the
// same codec can judge every reply (bridge_vectors.c check). See the format
// in bridge_vectors.c.
//
//   vvp tb_bridge.vvp +stim=<stimulus> +out=<log>
`timescale 1ns / 1ps
`default_nettype none
module tb_bridge;
    reg clk = 1'b0;
    always #18.5 clk = ~clk; // 27 MHz
    reg [63:0] tick = 64'd0;  // the bridge's tick counts the same edges from 0
    always @(posedge clk) tick <= tick + 64'd1;

    localparam integer BIT = 9; // 3 Mbaud

    reg        host_tx = 1'b1;
    wire       bridge_tx, crank, cam;
    wire [3:0] analog;
    reg  [5:0] inj = 6'd0;
    wire [5:0] led;
    top dut (.clk(clk), .uart_rx(host_tx), .uart_tx(bridge_tx), .crank(crank), .cam(cam),
             .analog(analog), .inj(inj), .led(led));

    integer out, stim, r;

    // ---------------------------------------------------------------- receiving the bridge's frames
    reg [7:0]  fr [0:1023];
    integer    fr_n = 0, fr_need = 0, frames = 0, i;
    reg [63:0] fr_tick = 64'd0;
    reg [7:0]  rb;
    initial begin : rx_loop
        forever begin
            @(negedge bridge_tx);           // start bit
            if (fr_n == 0) fr_tick = tick;
            repeat (BIT / 2) @(posedge clk);
            for (i = 0; i < 8; i = i + 1) begin
                repeat (BIT) @(posedge clk);
                rb[i] = bridge_tx;
            end
            repeat (BIT) @(posedge clk);    // stop bit
            if (fr_n == 0 && rb != 8'hA5) begin
                $fdisplay(out, "# junk from the bridge: %02x", rb);
            end else begin
                fr[fr_n] = rb;
                fr_n = fr_n + 1;
                if (fr_n == 7) fr_need = 9 + {fr[6], fr[5]};
                if (fr_n >= 7 && fr_n == fr_need) begin
                    $fwrite(out, "rx %0d %0d", fr_tick, fr_n);
                    for (i = 0; i < fr_n; i = i + 1) $fwrite(out, " %02x", fr[i]);
                    $fwrite(out, "\n");
                    fr_n = 0;
                    frames = frames + 1;
                end
            end
        end
    end

    // ---------------------------------------------------------------- sending bytes
    task send_byte(input [7:0] v);
        integer k;
        begin
            host_tx = 1'b0;
            repeat (BIT) @(posedge clk);
            for (k = 0; k < 8; k = k + 1) begin
                host_tx = v[k];
                repeat (BIT) @(posedge clk);
            end
            host_tx = 1'b1;
            repeat (BIT) @(posedge clk);
        end
    endtask

    // ---------------------------------------------------------------- injector pulses
    reg [63:0] p_at [0:5];
    reg [31:0] p_w  [0:5];
    reg [5:0]  p_on = 6'd0;
    integer ch;
    initial for (ch = 0; ch < 6; ch = ch + 1) begin p_at[ch] = 64'hFFFFFFFFFFFFFFFF; p_w[ch] = 0; end
    always @(posedge clk) begin : pulses
        integer c;
        for (c = 0; c < 6; c = c + 1) begin
            if (tick == p_at[c]) begin
                #1 inj[c] = 1'b1;
                $fdisplay(out, "pulse %0d %0d %0d", c, tick, p_w[c]);
            end
            if (tick == p_at[c] + p_w[c]) begin
                #1 inj[c] = 1'b0;
                p_at[c] = 64'hFFFFFFFFFFFFFFFF;
            end
        end
    end

    // ---------------------------------------------------------------- crank and cam edges
    reg crank_d = 1'b0, cam_d = 1'b0;
    integer crank_n = 0, cam_n = 0;
    always @(posedge clk) begin
        crank_d <= crank;
        cam_d <= cam;
        if (crank && !crank_d) crank_n = crank_n + 1;
        if (cam && !cam_d) cam_n = cam_n + 1;
    end

    // ---------------------------------------------------------------- the script
    reg [8*16-1:0] cmd;
    reg [8*256-1:0] stim_path, out_path;
    integer n, b, cycles, c0, dly, w, before, waited;
    reg [7:0] bytes [0:1023];
    initial begin
        if (!$value$plusargs("stim=%s", stim_path) || !$value$plusargs("out=%s", out_path)) begin
            $display("usage: vvp tb_bridge.vvp +stim=<file> +out=<file>");
            $finish;
        end
        stim = $fopen(stim_path, "r");
        out = $fopen(out_path, "w");
        repeat (40) @(posedge clk); // power-on reset
        forever begin
            r = $fscanf(stim, "%s", cmd);
            if (r != 1 || cmd == "end") begin
                $fclose(out);
                $finish;
            end else if (cmd == "junk" || cmd == "send") begin
                r = $fscanf(stim, "%d", n);
                for (b = 0; b < n; b = b + 1) r = $fscanf(stim, "%h", bytes[b]);
                before = frames;
                @(posedge clk);
                if (cmd == "send") $fdisplay(out, "sent %0d", tick);
                for (b = 0; b < n; b = b + 1) send_byte(bytes[b]);
                if (cmd == "send") begin
                    waited = 0;
                    while (frames == before && waited < 3000000) begin
                        @(posedge clk);
                        waited = waited + 1;
                    end
                    if (frames == before) $fdisplay(out, "timeout");
                    repeat (2 * BIT) @(posedge clk);
                end
            end else if (cmd == "idle") begin
                r = $fscanf(stim, "%d", cycles);
                repeat (cycles) @(posedge clk);
            end else if (cmd == "pulse") begin
                r = $fscanf(stim, "%d %d %d", c0, dly, w);
                p_w[c0] = w;
                p_at[c0] = tick + dly;
            end else if (cmd == "edges") begin
                $fdisplay(out, "edges %0d %0d", crank_n, cam_n);
                crank_n = 0;
                cam_n = 0;
            end else begin
                $display("tb_bridge: unknown command %0s", cmd);
                $finish;
            end
        end
    end
endmodule
`default_nettype wire
