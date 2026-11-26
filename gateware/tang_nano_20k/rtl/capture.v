// SPDX-License-Identifier: Apache-2.0
// Injector pulse capture (docs/bridge-protocol.md §3.3).
//
// Each channel's input is synchronised with two flip-flops. polarity bit k =
// 1 means channel k's pulse is high. On the pulse's leading edge the low 32
// bits of the tick are kept as its start; on its trailing edge the width in
// ticks is computed and the capture {width, start, channel} (72 bits, the
// wire's byte order) waits for the FIFO. Both edges are seen SYNC_DELAY
// ticks late, so the width is exact and the start is SYNC_DELAY late.
//
// Captures run only while `run` is high and the channel's mask bit is set.
// A capture that finds the FIFO full, or that completes while the channel's
// previous one is still waiting, is lost and reported by `lost`.
`default_nettype none
module capture #(
    parameter integer N = 6,
    parameter integer DEPTH_LOG2 = 5          // 32 captures
) (
    input  wire              clk,
    input  wire              rst,
    input  wire              run,
    input  wire              flush,           // empty the FIFO
    input  wire [N-1:0]      in,
    input  wire [7:0]        mask,
    input  wire [7:0]        polarity,
    input  wire [63:0]       tick,
    output wire [DEPTH_LOG2:0] count,
    output reg  [71:0]       head = 72'd0,    // the oldest capture, valid while count > 0
    input  wire              pop,
    output reg               lost = 1'b0      // one cycle per capture lost
);
    localparam integer DEPTH = 1 << DEPTH_LOG2;
    localparam [DEPTH_LOG2:0] FULL = DEPTH;

    reg [N-1:0] s1 = {N{1'b0}}, s2 = {N{1'b0}}, prev = {N{1'b0}};
    reg [N-1:0] in_pulse = {N{1'b0}}, pending = {N{1'b0}};
    reg [31:0]  start [0:N-1];
    reg [71:0]  done  [0:N-1];
    reg [71:0]  mem   [0:DEPTH-1];
    reg [DEPTH_LOG2:0] wr = 0, rd = 0;
    assign count = wr - rd;

    wire [N-1:0] active = s2 ^ ~polarity[N-1:0];

    // The lowest pending channel goes to the FIFO first.
    integer k;
    reg found;
    reg [7:0] pick;
    always @(*) begin
        found = 1'b0;
        pick = 8'd0;
        for (k = N - 1; k >= 0; k = k - 1)
            if (pending[k]) begin
                found = 1'b1;
                pick = k[7:0];
            end
    end

    integer c;
    always @(posedge clk) begin
        s1 <= in;
        s2 <= s1;
        lost <= 1'b0;
        if (rst || flush || !run) begin
            prev     <= active;
            in_pulse <= {N{1'b0}};
            pending  <= {N{1'b0}};
            if (rst || flush) rd <= wr;
        end else begin
            prev <= active;
            for (c = 0; c < N; c = c + 1) begin
                if (mask[c] && active[c] && !prev[c]) begin
                    start[c]    <= tick[31:0];
                    in_pulse[c] <= 1'b1;
                end
                if (in_pulse[c] && !active[c] && prev[c]) begin
                    in_pulse[c] <= 1'b0;
                    if (pending[c] && !(found && pick == c)) begin
                        lost <= 1'b1; // the previous one has not left yet
                    end else begin
                        done[c]    <= {tick[31:0] - start[c], start[c], c[7:0]};
                        pending[c] <= 1'b1;
                    end
                end
            end
            if (found) begin
                if (count == FULL && !pop) begin
                    lost <= 1'b1;
                end else begin
                    mem[wr[DEPTH_LOG2-1:0]] <= done[pick];
                    wr <= wr + 1'b1;
                end
                // cleared unless the same channel completed again in this cycle
                if (!(in_pulse[pick] && !active[pick] && prev[pick])) pending[pick] <= 1'b0;
            end
        end
        if (pop && count != 0) rd <= rd + 1'b1;
        head <= mem[(pop && count != 0) ? rd[DEPTH_LOG2-1:0] + 1'b1 : rd[DEPTH_LOG2-1:0]];
    end
endmodule
`default_nettype wire
