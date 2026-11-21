"""Generate the Phase 4 signed blind course (course-05 I2C NAK troubleshooting).

The teacher signing key stays local in .work/; only the public key lives in the
repository. Re-running regenerates the package deterministically except for the
Ed25519 signature, which is derived from the local private key.
"""
import hashlib, json, shutil
from pathlib import Path
from cryptography.hazmat.primitives import serialization
ROOT = Path(__file__).resolve().parents[2]
COURSE = ROOT / 'examples/virtual/courses/course-05-i2c-nak-troubleshooting'

MASTER = r'''// Course 05 fixture: open-drain I2C temperature monitor with an alarm LED.
// The sensor sits at 0x48; register 0x00 holds the temperature byte. The alarm
// lights when the reading exceeds 25 degrees. SDA is split into an open-drain
// output (0 = pull low, 1 = release to the board pull-up) plus a sense input,
// so the ACK slot really travels through the 13-state bus.
module i2c_alarm_monitor(
    input  wire        clock_in,
    input  wire        reset_n,
    output reg         i2c_scl,
    output reg         i2c_sda,
    input  wire        i2c_sda_in,
    output reg  [4:0]  led_n
);
    localparam int HALF_CYCLES = 8;
    localparam [7:0] ADDR_READ = 8'h91;    // 7'h48 << 1 | read
    localparam [31:0] MAX_RETRIES = 1;     // a single unacknowledged frame parks the bus
    localparam [7:0]  ALARM_THRESHOLD = 8'd25;

    reg [31:0] cyc = 0;
    reg [7:0]  step = 8'd0;
    reg        done = 0, error_state = 0;
    reg [31:0] retries = 0;
    reg [7:0]  value = 0;

    function automatic [1:0] bus(input [7:0] s);   // {scl, sda}
        begin
            bus = 2'b11;
            if (s == 8'd0)       bus = 2'b11;                        // idle
            else if (s == 8'd1)  bus = 2'b10;                        // START
            else if (s >= 8'd2 && s <= 8'd17)
                bus = {s[0], ADDR_READ[7 - ((s - 8'd2) >> 1)]};       // address + R/W
            else if (s == 8'd18) bus = 2'b01;                        // ACK slot low
            else if (s == 8'd19) bus = 2'b11;                        // ACK sample
            else if (s >= 8'd20 && s <= 8'd35) bus = {s[0], 1'b1};   // read byte
            else if (s == 8'd36) bus = 2'b01;                        // NAK low
            else if (s == 8'd37) bus = 2'b11;                        // NAK high
            else if (s == 8'd38) bus = 2'b00;                        // STOP prep
            else if (s == 8'd39) bus = 2'b10;
            else if (s == 8'd40) bus = 2'b11;                        // STOP
        end
    endfunction

    always @(*) begin
        {i2c_scl, i2c_sda} = bus(step);
    end

    always @(posedge clock_in or negedge reset_n) begin
        if (!reset_n) begin
            cyc <= 0; step <= 8'd0; done <= 0; error_state <= 0; retries <= 0;
            value <= 0; led_n <= 5'b11111;
        end else if (!done && !error_state) begin
            if (cyc != HALF_CYCLES-1) cyc <= cyc + 1;
            else begin
                cyc <= 0;
                if (step == 8'd19) begin
                    if (i2c_sda_in) begin
                        if (retries < MAX_RETRIES-1) begin
                            retries <= retries + 1; step <= 8'd0;
                        end else begin
                            error_state <= 1; step <= 8'd41;
                            led_n <= 5'b11111;   // bus parked: no alarm, no evidence
                        end
                    end else step <= 8'd20;
                end else if (step >= 8'd20 && step <= 8'd35 && step[0]) begin
                    value <= {value[6:0], i2c_sda_in}; step <= step + 8'd1;
                end else if (step == 8'd40) begin
                    done <= 1; step <= 8'd41;
                    // bit0: alarm (active low), bits 4:1: inverted low nibble.
                    led_n <= {~value[3:0], (value > ALARM_THRESHOLD) ? 1'b0 : 1'b1};
                end else step <= step + 8'd1;
            end
        end
    end
endmodule
'''


def read(p):
    return json.loads(Path(p).read_text(encoding='utf-8-sig'))


def write(p, value):
    p = Path(p)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def main():
    COURSE.mkdir(parents=True, exist_ok=True)
    board = read(ROOT / 'registry/boards/tang-nano-9k.phase4.virtual.json')
    board['version'] = '4.0.0-teaching-i2c'
    board_path = COURSE / 'board.json'
    write(board_path, board)

    cst = ROOT / 'examples/virtual/phase4/i2c-temp/i2c_temp.cst'
    shutil.copyfile(cst, COURSE / 'counter.cst')
    (COURSE / 'broken.sv').write_text(MASTER, encoding='utf-8')

    project = read(ROOT / 'examples/virtual/phase4/i2c-temp/virtual.project.json')
    project.update(source_files=['broken.sv'], top_module='i2c_alarm_monitor', cst_path='counter.cst',
                   board_path=str(board_path), project_root=str(COURSE))
    project['fault_profile']['board_ref'] = board['id'] + '@' + board['version']
    project['fault_profile']['id'] = 'course05-teacher-plan'
    project['fault_profile']['faults'] = []
    write(COURSE / 'virtual.project.json', project)

    secret = dict(id='secret-nak-7f4', enabled=True, visibility='teacher_only', hook='peripheral.event',
                  target=dict(kind='component', id='i2c0'),
                  effect=dict(type='i2c_nak', phase='address', nth_transaction=1),
                  trigger=dict(kind='permanent', start_tick='0'))
    plan = read(ROOT / 'examples/virtual/phase4/i2c-temp/virtual.project.json')['fault_profile']
    plan.update(board_ref=board['id'] + '@' + board['version'], id='course05-teacher-plan',
                rng=dict(algorithm='pcg32-v1', seed='9500123', streams='by_fault_id'), faults=[secret])
    write(COURSE / 'faults.private.json', plan)

    public = dict(schema='sigflow.course-task.v1', id='lesson-5',
                  title='I2C 总线应答异常：告警灯不亮',
                  objective='用只读探针测量 SCL/SDA，定位第 9 拍应答槽异常并恢复总线后提交修复',
                  allowed_observations=['pad.35', 'pad.36', 'pad.37', 'i2c0.SDA', 'i2c0.SCL',
                                        'port:led_n[0]', 'port:led_n[4]'],
                  public_tests=['reset_then_step', 'ticks_bounded'], hint_levels=['L1', 'L2', 'L3', 'L4'],
                  mode='student_blind_test',
                  clock_fixture=dict(frequency_hz='27000000', physical_board_oscillator_hz='27000000', teaching_override=False))
    write(COURSE / 'task.public.json', public)

    rubric = dict(schema='sigflow.course-rubric.v1',
                  cause=dict(location='i2c0', type='i2c_nak'),
                  hidden_seeds=['73191', '88703', '95077'],
                  project='virtual.project.json',
                  repair_kind='physical_service',
                  allowed_repair='i2c-sda-pullup',
                  assertion='i2c_ack_restored_and_alarm',
                  required_observations=['pad.35', 'pad.36'],
                  hints=['观察告警灯与总线是否始终没有变化。',
                         '在 SCL、SDA 两个物理脚上同时放只读探针，比较第 9 拍的电平。',
                         '确认应答槽期间 SDA 没有被任何驱动拉低，再检查外部上拉与器件应答。',
                         '恢复 SDA 上拉/应答后，用三组隐藏激励确认主机读到温度并点亮告警。'])
    write(COURSE / 'rubric.private.json', rubric)

    key_file = ROOT / '.work/p2/teacher-key.pem'
    key = serialization.load_pem_private_key(key_file.read_bytes(), None)
    names = ['task.public.json', 'faults.private.json', 'rubric.private.json',
             'virtual.project.json', 'board.json', 'broken.sv', 'counter.cst']
    manifest = dict(schema='sigflow.signed-course.v1', algorithm='Ed25519',
                    files={n: hashlib.sha256((COURSE / n).read_bytes()).hexdigest() for n in names})
    canonical = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode()
    manifest['signature'] = key.sign(canonical).hex()
    write(COURSE / 'manifest.private.json', manifest)
    print('course-05 generated and signed:', COURSE)


if __name__ == '__main__':
    main()
