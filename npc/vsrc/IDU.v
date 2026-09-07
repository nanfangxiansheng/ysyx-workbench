// IDU.v - Instruction Decode Unit
module IDU (
    input  [31:0] inst,
    output [6:0]  opcode,
    output [4:0]  rd,
    output [2:0]  funct3,
    output [4:0]  rs1,
    output [4:0]  rs2,
    output [6:0]  funct7,
    output [31:0] imm_i,
    output [31:0] imm_u,
    output [31:0] imm_s,
    output [31:0] imm_j,
    output [31:0] imm_b
);
    
    // 指令字段提取
    assign opcode = inst[6:0];
    assign rd     = inst[11:7];
    assign funct3 = inst[14:12];
    assign rs1    = inst[19:15];
    assign rs2    = inst[24:20];
    assign funct7 = inst[31:25];
    
    // I-type立即数（带符号扩展）
    assign imm_i = {{20{inst[31]}}, inst[31:20]};
    
    // U-type立即数（lui）
    assign imm_u = {inst[31:12], 12'b0};//本身imm_u就是左移动12位后的结果
    
    // S-type立即数（带符号扩展）
    assign imm_s = {{20{inst[31]}}, inst[31:25], inst[11:7]};

    // J-type立即数（jal）: 4段乱序存放, 拼回时最低位补0, 符号扩展
    // imm[20]=inst[31], imm[10:1]=inst[30:21], imm[11]=inst[20], imm[19:12]=inst[19:12]
    assign imm_j = {{12{inst[31]}}, inst[19:12], inst[20], inst[30:21], 1'b0};

    // B-type立即数（beq/bne等分支）: 最低位补0, 符号扩展
    // imm[12]=inst[31], imm[11]=inst[7], imm[10:5]=inst[30:25], imm[4:1]=inst[11:8]
    assign imm_b = {{19{inst[31]}}, inst[31], inst[7], inst[30:25], inst[11:8], 1'b0};
    
endmodule
