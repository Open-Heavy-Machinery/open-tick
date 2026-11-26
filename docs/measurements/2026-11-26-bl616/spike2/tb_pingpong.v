// iverilog testbench for pingpong.v: a fast UART (4 clocks per bit) and a
// short step (200 clocks). Prints SIM PASS or SIM FAIL.
`timescale 1ns/1ps
`default_nettype none
module tb_pingpong;
    localparam integer CPB = 4;
    localparam integer STEP = 200;
    reg clk = 0;
    always #1 clk = ~clk;
    reg rx = 1'b1;
    wire tx;
    wire [5:0] led;
    pingpong #(.CLKS_PER_BIT(CPB), .STEP_CLKS(STEP), .SP_LOG2(4), .TX_LOG2(6)) dut (
        .clk(clk), .uart_rx(rx), .uart_tx(tx), .led(led));

    integer fails = 0;
    task check(input cond, input [8*64-1:0] what);
        begin
            if (!cond) begin
                $display("FAIL: %0s", what);
                fails = fails + 1;
            end
        end
    endtask

    task send_byte(input [7:0] b);
        integer i;
        begin
            rx = 1'b0; repeat (CPB) @(posedge clk);
            for (i = 0; i < 8; i = i + 1) begin rx = b[i]; repeat (CPB) @(posedge clk); end
            rx = 1'b1; repeat (CPB) @(posedge clk);
        end
    endtask

    // Reply capture: sample each bit in its middle.
    reg [7:0] rep [0:255];
    integer nrep = 0;
    initial begin : capture
        integer i;
        reg [7:0] b;
        forever begin
            @(negedge tx);
            repeat (CPB / 2) @(posedge clk);
            for (i = 0; i < 8; i = i + 1) begin
                repeat (CPB) @(posedge clk);
                b[i] = tx;
            end
            repeat (CPB) @(posedge clk);
            rep[nrep] = b;
            nrep = nrep + 1;
        end
    end

    reg [7:0] sum;
    reg [31:0] step2, step3;
    task request(input [7:0] typ, input [7:0] seq, input integer cnt, input integer rlen, input bad);
        integer i;
        reg [7:0] b;
        begin
            nrep = 0;
            sum = 0;
            send_byte(8'hA5);
            send_byte(typ); sum = sum + typ;
            send_byte(seq); sum = sum + seq;
            send_byte(cnt[7:0]); sum = sum + cnt[7:0];
            send_byte(cnt[15:8]); sum = sum + cnt[15:8];
            send_byte(rlen[7:0]); sum = sum + rlen[7:0];
            send_byte(rlen[15:8]); sum = sum + rlen[15:8];
            for (i = 0; i < cnt; i = i + 1) begin
                b = 8'h10 + i;
                send_byte(b); sum = sum + b;
            end
            send_byte(bad ? sum + 8'd1 : sum);
            wait (nrep == 28 + rlen + 1);
        end
    endtask

    function [47:0] f48(input integer at);
        f48 = {rep[at+5], rep[at+4], rep[at+3], rep[at+2], rep[at+1], rep[at]};
    endfunction
    function [15:0] f16(input integer at);
        f16 = {rep[at+1], rep[at]};
    endfunction

    task check_reply(input [7:0] seq, input integer rlen);
        integer i;
        reg [7:0] s;
        begin
            check(rep[0] == 8'h5A, "sync byte");
            check(rep[1] == seq, "seq echoed");
            check(f48(9) > f48(3), "t_tx after t_rx");
            check(f16(24) == rlen, "rlen echoed");
            for (i = 0; i < rlen; i = i + 1) check(rep[28 + i] == i[7:0], "filler");
            s = 0;
            for (i = 1; i < 28 + rlen; i = i + 1) s = s + rep[i];
            check(rep[28 + rlen] == s, "reply checksum");
        end
    endtask

    initial begin
        repeat (20) @(posedge clk);
        // 1. size probe, not started: no underruns, fill 0
        request(8'd2, 8'd7, 3, 5, 1'b0);
        check_reply(8'd7, 5);
        check(rep[2][0] == 1'b1, "request 1 checksum ok");
        check(f16(21) == 0, "no underrun before start");
        check(f16(19) == 0, "fill 0 before start");
        // 2. three setpoints: consumption starts
        request(8'd1, 8'd8, 6, 0, 1'b0);
        check_reply(8'd8, 0);
        check(f16(19) == 3, "three setpoints queued");
        step2 = {rep[18], rep[17], rep[16], rep[15]};
        // 3. six steps later: three consumed, then underruns
        repeat (6 * STEP) @(posedge clk);
        request(8'd2, 8'd9, 0, 0, 1'b0);
        check_reply(8'd9, 0);
        check(f16(19) == 0, "FIFO drained");
        check(f16(26) == 16'h1514, "last setpoint applied, little-endian, in order");
        step3 = {rep[18], rep[17], rep[16], rep[15]};
        // every step since the start found a setpoint or counted an underrun
        check(f16(21) + 3 >= step3 - step2 - 1 && f16(21) + 3 <= step3 - step2 + 1, "underruns = steps - setpoints");
        $display("steps between replies %0d, setpoints 3, underruns %0d", step3 - step2, f16(21));
        // 4. a bad checksum is answered, flagged and counted
        request(8'd2, 8'd10, 2, 0, 1'b1);
        check_reply(8'd10, 0);
        check(rep[2][0] == 1'b0, "bad checksum flagged");
        check(rep[23] == 8'd1, "checksum errors counted");
        if (fails == 0) $display("SIM PASS"); else $display("SIM FAIL (%0d)", fails);
        $finish;
    end
    initial begin #2000000; $display("SIM FAIL (timeout)"); $finish; end
endmodule
`default_nettype wire
