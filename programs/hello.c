static int sc(int n,int a,int b,int c){int r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"b"(a),"c"(b),"d"(c));return r;}
void _start(void){
  const char*m="Hello from a real Linux ELF program running on AxisOS!\n";
  int l=0; while(m[l])l++;
  sc(4,1,(int)m,l);
  sc(1,0,0,0);
}
