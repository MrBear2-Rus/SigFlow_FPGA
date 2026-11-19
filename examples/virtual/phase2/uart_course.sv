// Explicit 1MHz teaching clock / 100k baud fixture. 8N1 checker and wire echo.
module p2_uart_course(input wire clock_in,reset_n,key_n,input wire [1:0] dip,
    input wire uart_rx,output wire uart_tx,output wire [5:0] led_n,
    output wire [7:0] seg_n,output wire [3:0] dig_en);
    localparam integer DIV=10;
    reg [1:0] state;
    reg [7:0] good_count,bad_count,data;
    reg [3:0] countdown,bit_index;
    always @(posedge clock_in or negedge reset_n)
        if(!reset_n) begin state<=0;good_count<=0;bad_count<=0;data<=0;countdown<=0;bit_index<=0;end
        else case(state)
            0: if(!uart_rx) begin state<=1;countdown<=DIV/2-1;end
            1: if(countdown!=0)countdown<=countdown-1;else if(!uart_rx)begin state<=2;countdown<=DIV-1;bit_index<=0;end else state<=0;
            2: if(countdown!=0)countdown<=countdown-1;else begin data[bit_index]<=uart_rx;countdown<=DIV-1;if(bit_index==7)state<=3;else bit_index<=bit_index+1;end
            3: if(countdown!=0)countdown<=countdown-1;else begin if(uart_rx)good_count<=good_count+1;else bad_count<=bad_count+1;state<=0;end
        endcase
    assign uart_tx=uart_rx;
    assign led_n=~{bad_count[2:0],good_count[2:0]};
    assign seg_n=8'hff;assign dig_en=4'b0;
endmodule
