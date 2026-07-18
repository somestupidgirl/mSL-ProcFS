#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>      /* major()/minor() */
#include <sys/stat.h>
#include <stdlib.h>
static void dev_family(const char *in,char *out,size_t cap){size_t i=0;
  while(in[i]&&i+1<cap&&!(in[i]>='0'&&in[i]<='9')){out[i]=in[i];i++;}out[i]=0;
  if(i==0){strncpy(out,in,cap);return;}
  while(i>1&&(out[i-1]=='.'||out[i-1]=='_'||out[i-1]=='-'))out[--i]=0;}
static int excl(const char*f){const char*p[]={"tty","pty","cu","disk","rdisk",0};
  const char*e[]={"null","zero","mem","kmem","random","urandom","console","ptmx","stdin","stdout","stderr","fd",0};
  for(int i=0;p[i];i++)if(!strncmp(f,p[i],strlen(p[i])))return 1;
  for(int i=0;e[i];i++)if(!strcmp(f,e[i]))return 1;return 0;}
struct r{int mn;char n[64];};
int main(void){DIR*d=opendir("/dev");struct dirent*de;struct r rows[128];int n=0;
  while((de=readdir(d))){if(de->d_name[0]=='.')continue;char p[1100];snprintf(p,sizeof p,"/dev/%s",de->d_name);
    struct stat st;if(lstat(p,&st)||!S_ISCHR(st.st_mode))continue;char f[64];dev_family(de->d_name,f,sizeof f);
    if(!f[0]||excl(f))continue;int mn=minor(st.st_rdev);int j;for(j=0;j<n;j++)if(!strcmp(rows[j].n,f)){if(mn<rows[j].mn)rows[j].mn=mn;break;}
    if(j==n&&n<128){rows[n].mn=mn;strncpy(rows[n].n,f,63);n++;}}
  closedir(d);for(int i=0;i<n;i++)printf("%3d %s\n",rows[i].mn,rows[i].n);return 0;}
