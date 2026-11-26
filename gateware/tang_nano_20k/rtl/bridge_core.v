// SPDX-License-Identifier: Apache-2.0
// The bridge's protocol engine (docs/bridge-protocol.md, version 1).
//
// - Messages: configure, start, stop and exchange, each answered once (§3).
//   A frame whose CRC failed, or of an unknown type, gets an exchange reply
//   with error.crc or error.state and the seq as received (§3.1).
// - States: idle, configured, running, stopped, aborted (§3.4).
// - The setpoint buffer: a FIFO of 12-byte setpoints in block RAM, for
//   contiguous steps ending at next_q - 1 (§3.2).
// - Steps: step k begins at t_start + k * step_ticks. Two ticks before each
//   boundary the head of the buffer is fetched; at the boundary it is applied
//   if it is the setpoint of that step, otherwise the step is an underrun and
//   the last setpoint is held (§2). A setpoint must therefore be queued at
//   least 2 ticks before its step.
// - Link timeout: link_timeout_steps boundaries without a valid frame abort
//   the run (§3.5).
// - Replies are assembled byte by byte, with the CRC computed on the way.
`default_nettype none
module bridge_core #(
    parameter integer TICK_HZ = 27000000,
    parameter integer N_CAPTURE = 6,
    parameter integer N_ANALOG = 4,
    parameter integer ANALOG_BITS = 16,
    parameter integer FIFO_LOG2 = 6,        // 64 setpoints
    parameter integer CAP_LOG2 = 5,         // the capture FIFO's size (capture.v)
    parameter integer MAX_REPLY_CAPS = 32,
    parameter integer MIN_STEP_TICKS = 2700 // 100 us at 27 MHz
) (
    input  wire         clk,
    input  wire         rst,
    // frame_rx
    output wire         accept,
    input  wire         msg_valid,
    input  wire         msg_crc_ok,
    input  wire [7:0]   msg_type,
    input  wire [15:0]  msg_seq,
    input  wire [15:0]  msg_len,
    input  wire [63:0]  msg_t2,
    output reg  [8:0]   raddr = 9'd0,
    input  wire [7:0]   rdata,
    // uart_tx
    output reg  [7:0]   tx_data = 8'd0,
    output reg          tx_start = 1'b0,
    input  wire         tx_busy,
    // time and state
    output reg  [63:0]  tick = 64'd0,
    output wire         run,
    output reg  [2:0]   state = 3'd0,
    // the applied setpoint, zero at rest
    output wire [31:0]  phase_inc,
    output wire [63:0]  analog,             // four u16, channel 0 in the low bits
    // configuration
    output reg  [7:0]   crank_teeth = 8'd60,
    output reg  [7:0]   crank_missing = 8'd2,
    output reg  [31:0]  crank_offset = 32'd0,
    output reg  [31:0]  cam_start = 32'd0,
    output reg  [31:0]  cam_end = 32'd0,
    output reg  [7:0]   capture_mask = 8'd0,
    output reg  [7:0]   capture_polarity = 8'd0,
    // capture FIFO
    input  wire [CAP_LOG2:0] cap_count,
    input  wire [71:0]  cap_head,
    output reg          cap_pop = 1'b0,
    output reg          cap_flush = 1'b0,
    input  wire         cap_lost
);
`include "crc16.vh"
    localparam [2:0] S_IDLE = 3'd0, S_CONFIGURED = 3'd1, S_RUNNING = 3'd2, S_STOPPED = 3'd3, S_ABORTED = 3'd4;
    localparam [7:0] T_EXCHANGE = 8'h01, T_CONFIGURE = 8'h02, T_START = 8'h03, T_STOP = 8'h04, T_REPLY = 8'h80;
    localparam integer FIFO_DEPTH = 1 << FIFO_LOG2;
    localparam [31:0] TICK_HZ_W = TICK_HZ;
    localparam [7:0]  N_CAPTURE_W = N_CAPTURE, N_ANALOG_W = N_ANALOG, ANALOG_BITS_W = ANALOG_BITS;
    localparam [CAP_LOG2:0] MAX_CAPS_W = MAX_REPLY_CAPS;
    localparam [FIFO_LOG2:0] FIFO_FULL = FIFO_DEPTH;

    // ------------------------------------------------------------ the control FSM
    localparam [3:0] C_IDLE = 4'd0, C_DISPATCH = 4'd1, C_READ = 4'd2, C_READ_WAIT = 4'd3, C_READ_TAKE = 4'd4,
                     C_EX_CHECK = 4'd5, C_EX_SP = 4'd6, C_CFG = 4'd7, C_BUILD = 4'd8, C_TX = 4'd9,
                     C_TX_WAIT = 4'd10;
    localparam [1:0] R_EX_HDR = 2'd0, R_EX_SP = 2'd1, R_CFG = 2'd2;

    reg [3:0]   cs = C_IDLE;
    reg [1:0]   reading = R_EX_HDR;
    reg [7:0]   m_type = 8'd0;
    reg [15:0]  m_seq = 16'd0, m_len = 16'd0;
    reg [63:0]  m_t2 = 64'd0;
    reg         m_crc_ok = 1'b0;
    reg [8:0]   rd_i = 9'd0;            // next payload byte to read
    reg [4:0]   rd_left = 5'd0;         // bytes left in the current field group
    reg [175:0] sh = 176'd0;            // bytes read, newest at the top
    reg [31:0]  first_step = 32'd0;
    reg [7:0]   n_sp = 8'd0, sp_i = 8'd0;

    assign accept = (cs == C_IDLE) && !msg_valid;

    // ------------------------------------------------------------ configuration and run state
    reg [31:0]  step_ticks = 32'd27000;
    reg [15:0]  link_timeout = 16'd100;
    reg [63:0]  t_start = 64'd0;
    reg [63:0]  next_boundary = 64'd0;
    reg [31:0]  pend = 32'd0;           // the step that begins at next_boundary
    reg [31:0]  step = 32'd0;           // the current step
    reg [15:0]  since_rx = 16'd0;       // boundaries since the last valid frame
    reg [31:0]  underruns = 32'd0;
    reg [15:0]  last_seq = 16'd0;
    reg         seq_valid = 1'b0;

    // ------------------------------------------------------------ setpoint FIFO
    reg [95:0]  fifo [0:FIFO_DEPTH-1];
    reg [FIFO_LOG2-1:0] wr_ptr = 0, rd_ptr = 0, pre_ptr = 0;
    reg [FIFO_LOG2:0]   fill = 0;
    reg [31:0]  next_q = 32'd0;         // the step of the next setpoint queued
    wire [31:0] head_step = next_q - {{(31-FIFO_LOG2){1'b0}}, fill};
    reg [95:0]  rd_data = 96'd0;
    reg         pre_ok = 1'b0;
    reg [95:0]  applied = 96'd0;

    always @(posedge clk) rd_data <= fifo[pre_ptr];

    assign run = (state == S_RUNNING);
    assign phase_inc = run ? applied[31:0] : 32'd0;
    assign analog = run ? applied[95:32] : 64'd0;

    // ------------------------------------------------------------ errors since the last exchange reply
    reg         err_late = 1'b0, err_ovf = 1'b0;
    reg [15:0]  err_count = 16'd0;
    reg         now_crc = 1'b0, now_seq = 1'b0, now_state = 1'b0;

    // ------------------------------------------------------------ reply
    reg [7:0]   r_type = 8'd0;
    reg [15:0]  r_len = 16'd0;
    reg [351:0] r_sh = 352'd0;          // the fixed payload, next byte in the low bits
    reg [15:0]  r_fixed = 16'd0;        // its length
    reg [15:0]  ti = 16'd0;             // byte of the frame being sent
    reg [15:0]  r_crc = 16'hFFFF;
    reg [3:0]   cap_j = 4'd0;
    reg [71:0]  cap_sh = 72'd0;
    reg [7:0]   r_status = 8'd0;

    // ------------------------------------------------------------ one byte of the reply frame
    reg [7:0] b;
    always @(*) begin
        if (ti == 16'd0) b = 8'hA5;
        else if (ti == 16'd1) b = 8'd1;
        else if (ti == 16'd2) b = r_type;
        else if (ti == 16'd3) b = m_seq[7:0];
        else if (ti == 16'd4) b = m_seq[15:8];
        else if (ti == 16'd5) b = r_len[7:0];
        else if (ti == 16'd6) b = r_len[15:8];
        else if (ti < 16'd7 + r_fixed) b = r_sh[7:0];
        else if (ti < 16'd7 + r_len) b = (cap_j == 4'd0) ? cap_head[7:0] : cap_sh[7:0];
        else if (ti == 16'd7 + r_len) b = r_crc[7:0];
        else b = r_crc[15:8];
    end

    // ------------------------------------------------------------ everything clocked
    reg push, pop, pop_late, clear_err, set_late, set_ovf;
    reg [1:0] err_inc;
    reg [16:0] err_sum;
    wire [CAP_LOG2:0] n_cap_now = (cap_count > MAX_CAPS_W) ? MAX_CAPS_W : cap_count;
    reg [95:0] push_word;
    reg [31:0] s_i;
    always @(posedge clk) begin
        tick     <= tick + 64'd1;
        tx_start <= 1'b0;
        cap_pop  <= 1'b0;
        cap_flush <= 1'b0;
        push = 1'b0;
        pop = 1'b0;
        pop_late = 1'b0;
        push_word = 96'd0;

        clear_err = 1'b0;
        set_late = 1'b0;
        set_ovf = cap_lost;
        err_inc = cap_lost ? 2'd1 : 2'd0;

        // ---- steps (§2)
        if (state == S_RUNNING) begin
            if (tick + 64'd2 == next_boundary) begin
                pre_ok  <= (fill != 0) && (head_step == pend);
                pre_ptr <= rd_ptr;
            end else if (tick == next_boundary) begin
                step <= pend;
                pend <= pend + 32'd1;
                next_boundary <= next_boundary + {32'd0, step_ticks};
                if (pre_ok) begin
                    applied <= rd_data;
                    pop = 1'b1;
                end else if (underruns != 32'hFFFFFFFF) begin
                    underruns <= underruns + 32'd1;
                end
                if (since_rx + 16'd1 >= link_timeout) begin
                    state <= S_ABORTED; // §3.5: the outputs go to rest with `run`
                end
                since_rx <= since_rx + 16'd1;
            end else if (tick + 64'd2 != next_boundary && tick + 64'd1 != next_boundary &&
                         fill != 0 && head_step < pend) begin
                pop_late = 1'b1; // a setpoint queued too late for its step
            end
        end

        // ---- control
        case (cs)
        C_IDLE:
            if (msg_valid) begin
                m_type   <= msg_type;
                m_seq    <= msg_seq;
                m_len    <= msg_len;
                m_t2     <= msg_t2;
                m_crc_ok <= msg_crc_ok;
                now_crc  <= 1'b0;
                now_seq  <= 1'b0;
                now_state <= 1'b0;
                r_status <= 8'd0;
                cs <= C_DISPATCH;
            end
        C_DISPATCH: begin
            rd_i <= 9'd0;
            if (!m_crc_ok) begin
                now_crc <= 1'b1;
                r_type <= T_EXCHANGE | T_REPLY;
                cs <= C_BUILD;
            end else begin
                since_rx <= 16'd0; // a valid frame keeps the link alive (§3.5)
                case (m_type)
                T_EXCHANGE: begin
                    r_type <= T_EXCHANGE | T_REPLY;
                    if (m_len < 16'd5) begin
                        now_state <= 1'b1;
                        cs <= C_BUILD;
                    end else begin
                        reading <= R_EX_HDR;
                        rd_left <= 5'd5;
                        cs <= C_READ;
                    end
                end
                T_CONFIGURE: begin
                    r_type <= T_CONFIGURE | T_REPLY;
                    if (m_len != 16'd22) begin
                        r_type <= T_EXCHANGE | T_REPLY;
                        now_state <= 1'b1;
                        cs <= C_BUILD;
                    end else begin
                        reading <= R_CFG;
                        rd_left <= 5'd22;
                        cs <= C_READ;
                    end
                end
                T_START: begin
                    r_type <= T_START | T_REPLY;
                    if (state != S_CONFIGURED || fill == 0 || head_step != 32'd0) begin
                        r_status <= 8'd1;
                    end else if (m_t2 + {32'd0, step_ticks} <= tick + 64'd4) begin
                        r_status <= 8'd2; // step 0 would already have begun
                    end else begin
                        t_start       <= m_t2 + {32'd0, step_ticks};
                        next_boundary <= m_t2 + {32'd0, step_ticks};
                        pend     <= 32'd0;
                        step     <= 32'd0;
                        since_rx <= 16'd0;
                        state    <= S_RUNNING;
                    end
                    cs <= C_BUILD;
                end
                T_STOP: begin
                    r_type <= T_STOP | T_REPLY;
                    if (state != S_RUNNING) begin
                        r_status <= 8'd1;
                    end else begin
                        state <= S_STOPPED;
                    end
                    cs <= C_BUILD;
                end
                default: begin
                    r_type <= T_EXCHANGE | T_REPLY;
                    now_state <= 1'b1;
                    cs <= C_BUILD;
                end
                endcase
            end
        end

        // ---- reading payload bytes: address, wait, take
        C_READ: begin
            raddr <= rd_i;
            cs <= C_READ_WAIT;
        end
        C_READ_WAIT:
            cs <= C_READ_TAKE;
        C_READ_TAKE: begin
            sh      <= {rdata, sh[175:8]};
            rd_i    <= rd_i + 9'd1;
            rd_left <= rd_left - 5'd1;
            if (rd_left != 5'd1) begin
                cs <= C_READ;
            end else begin
                case (reading)
                R_EX_HDR: cs <= C_EX_CHECK;
                R_EX_SP:  cs <= C_EX_SP;
                default:  cs <= C_CFG;
                endcase
            end
        end

        C_EX_CHECK: begin
            // sh[175:136] holds the 5 header bytes
            first_step <= sh[167:136];
            n_sp <= sh[175:168];
            sp_i <= 8'd0;
            if (state != S_CONFIGURED && state != S_RUNNING) begin
                now_state <= 1'b1;
                cs <= C_BUILD;
            end else if (sh[175:168] > 8'd36 || m_len != 16'd5 + 16'd12 * {8'd0, sh[175:168]}) begin
                now_state <= 1'b1; // malformed
                cs <= C_BUILD;
            end else if (seq_valid && m_seq == last_seq) begin
                cs <= C_BUILD; // a retransmission: answered, not applied
            end else if (sh[167:136] != next_q) begin
                now_seq <= 1'b1;
                cs <= C_BUILD;
            end else if (sh[175:168] == 8'd0) begin
                last_seq <= m_seq;
                seq_valid <= 1'b1;
                cs <= C_BUILD;
            end else begin
                last_seq <= m_seq;
                seq_valid <= 1'b1;
                reading <= R_EX_SP;
                rd_left <= 5'd12;
                cs <= C_READ;
            end
        end

        C_EX_SP: begin
            // sh[175:80] holds one setpoint, in wire order
            s_i = first_step + {24'd0, sp_i};
            next_q <= s_i + 32'd1;
            if (state == S_RUNNING && s_i < pend) begin
                set_late = 1'b1; // its step has begun
                err_inc = err_inc + 2'd1;
            end else if (fill == FIFO_FULL && !pop) begin
                now_seq <= 1'b1;   // more than the buffer holds
                err_inc = err_inc + 2'd1;
            end else begin
                push = 1'b1;
                push_word = sh[175:80];
            end
            sp_i <= sp_i + 8'd1;
            if (sp_i + 8'd1 == n_sp) begin
                cs <= C_BUILD;
            end else begin
                rd_left <= 5'd12;
                cs <= C_READ;
            end
        end

        C_CFG: begin
            // sh holds the 22 configure bytes, byte 0 at sh[7:0]
            r_type <= T_CONFIGURE | T_REPLY;
            if (state == S_RUNNING) begin
                r_status <= 8'd1;
            end else if (sh[31:0] < MIN_STEP_TICKS || sh[39:32] < 8'd3 || sh[47:40] >= sh[39:32] ||
                         sh[175:160] == 16'd0) begin
                r_status <= 8'd2;
            end else begin
                step_ticks       <= sh[31:0];
                crank_teeth      <= sh[39:32];
                crank_missing    <= sh[47:40];
                crank_offset     <= sh[79:48];
                cam_start        <= sh[111:80];
                cam_end          <= sh[143:112];
                capture_mask     <= sh[151:144];
                capture_polarity <= sh[159:152];
                link_timeout     <= sh[175:160];
                state     <= S_CONFIGURED;
                underruns <= 32'd0;
                step      <= 32'd0;
                pend      <= 32'd0;
                next_q    <= 32'd0;
                seq_valid <= 1'b0;
                applied   <= 96'd0;
                clear_err = 1'b1;
                cap_flush <= 1'b1;
            end
            cs <= C_BUILD;
        end

        // ---- reply: snapshot, then one byte at a time
        C_BUILD: begin
            ti    <= 16'd0;
            r_crc <= 16'hFFFF;
            cap_j <= 4'd0;
            case (r_type)
            (T_CONFIGURE | T_REPLY): begin
                r_fixed <= 16'd8;
                r_len   <= 16'd8;
                r_sh    <= {288'd0, ANALOG_BITS_W, N_ANALOG_W, N_CAPTURE_W, TICK_HZ_W, r_status};
            end
            (T_START | T_REPLY): begin
                r_fixed <= 16'd9;
                r_len   <= 16'd9;
                r_sh    <= {280'd0, t_start, r_status};
            end
            (T_STOP | T_REPLY): begin
                r_fixed <= 16'd9;
                r_len   <= 16'd9;
                r_sh    <= {280'd0, underruns, step, r_status};
            end
            default: begin // exchange reply
                r_fixed <= 16'd44;
                r_len   <= 16'd44 + 16'd9 * {{(15-CAP_LOG2){1'b0}}, n_cap_now};
                r_sh    <= {{{(7-CAP_LOG2){1'b0}}, n_cap_now},
                            applied, err_count,
                            {10'd0, now_state, (state == S_ABORTED), err_ovf, err_late, now_seq, now_crc},
                            underruns, {{(15-FIFO_LOG2){1'b0}}, fill}, {5'd0, state}, step, tick, m_t2};
                clear_err = 1'b1; // reported now
            end
            endcase
            cs <= C_TX;
        end
        C_TX:
            if (!tx_busy) begin
                tx_data  <= b;
                tx_start <= 1'b1;
                if (ti >= 16'd1 && ti < 16'd7 + r_len) r_crc <= crc16_byte(r_crc, b);
                if (ti >= 16'd7 && ti < 16'd7 + r_fixed) begin
                    r_sh <= r_sh >> 8;
                end else if (ti >= 16'd7 + r_fixed && ti < 16'd7 + r_len) begin
                    cap_sh <= (cap_j == 4'd0) ? (cap_head >> 8) : (cap_sh >> 8);
                    if (cap_j == 4'd8) begin
                        cap_j   <= 4'd0;
                        cap_pop <= 1'b1;
                    end else begin
                        cap_j <= cap_j + 4'd1;
                    end
                end
                ti <= ti + 16'd1;
                cs <= C_TX_WAIT;
            end
        C_TX_WAIT: // uart_tx raises busy one cycle after start
            if (tx_busy) cs <= (ti == 16'd9 + r_len) ? C_IDLE : C_TX;
        default:
            cs <= C_IDLE;
        endcase

        // ---- leaving the run: the buffer is emptied (§3.4)
        if (state == S_RUNNING && (cs == C_DISPATCH && m_crc_ok && m_type == T_STOP)) begin
            pop = 1'b0;
            push = 1'b0;
        end

        // ---- the FIFO, once per cycle
        if (push) begin
            fifo[wr_ptr] <= push_word;
            wr_ptr <= wr_ptr + 1'b1;
        end
        if (pop || pop_late) begin
            rd_ptr <= rd_ptr + 1'b1;
        end
        if (pop_late) begin
            set_late = 1'b1;
            err_inc = err_inc + 2'd1;
        end
        err_sum = (clear_err ? 17'd0 : {1'b0, err_count}) + {15'd0, err_inc};
        err_count <= err_sum[16] ? 16'hFFFF : err_sum[15:0];
        err_late  <= (clear_err ? 1'b0 : err_late) | set_late;
        err_ovf   <= (clear_err ? 1'b0 : err_ovf) | set_ovf;
        fill <= fill + (push ? 1'b1 : 1'b0) - ((pop || pop_late) ? 1'b1 : 1'b0);

        // configure, stop and abort empty the buffer
        if ((cs == C_CFG && state != S_RUNNING) ||
            (cs == C_DISPATCH && m_crc_ok && m_type == T_STOP && state == S_RUNNING) ||
            (state == S_RUNNING && tick == next_boundary && since_rx + 16'd1 >= link_timeout)) begin
            rd_ptr <= wr_ptr + (push ? 1'b1 : 1'b0);
            fill   <= 0;
        end

        if (rst) begin
            state <= S_IDLE;
            cs    <= C_IDLE;
            fill  <= 0;
            rd_ptr <= 0;
            wr_ptr <= 0;
        end
    end
endmodule
`default_nettype wire
