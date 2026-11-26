// SPDX-License-Identifier: Apache-2.0
// Rotary pattern generator (docs/bridge-protocol.md §3.2 and §3.4).
//
// A 48-bit phase accumulator gains `inc` every tick while `run` is high;
// 2^48 is one 720-degree cycle. Its top 32 bits (ph) drive two patterns:
//   crank: p = (ph - offset) mod 2^31 (360 degrees); tooth i = floor(p *
//          teeth / 2^31); high in the first half of each tooth's pitch, for
//          i < teeth - missing.
//   cam:   high while ph is in [cam_start, cam_end), wrapping if start > end.
// At rest (`run` low) the phase stops and both outputs hold their level.
`default_nettype none
module rotary (
    input  wire        clk,
    input  wire        rst,
    input  wire        run,
    input  wire [31:0] inc,
    input  wire [7:0]  teeth,
    input  wire [7:0]  missing,
    input  wire [31:0] offset,
    input  wire [31:0] cam_start,
    input  wire [31:0] cam_end,
    output reg         crank = 1'b0,
    output reg         cam = 1'b0,
    output wire [31:0] phase_hi
);
    reg [47:0] phase = 48'd0;
    assign phase_hi = phase[47:16];

    wire [31:0] rel  = phase_hi - offset;
    wire [38:0] prod = {8'd0, rel[30:0]} * {31'd0, teeth};
    wire [7:0]  idx  = prod[38:31];
    wire        second_half = prod[30];
    wire        tooth = (idx < (teeth - missing)) && !second_half;
    wire        in_cam = (cam_start <= cam_end) ? (phase_hi >= cam_start && phase_hi < cam_end)
                                                : (phase_hi >= cam_start || phase_hi < cam_end);

    always @(posedge clk) begin
        if (rst) begin
            phase <= 48'd0;
            crank <= 1'b0;
            cam   <= 1'b0;
        end else if (run) begin
            phase <= phase + {16'd0, inc};
            crank <= tooth;
            cam   <= in_cam;
        end
    end
endmodule
`default_nettype wire
