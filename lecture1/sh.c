#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/dirent.h>
#include <dirent.h>

// Simplifed xv6 shell.

#define MAXARGS 10
#define MAXCHAR 100

// All commands have at least a type. Have looked at the type, the code
// typically casts the *cmd to some specific cmd type.
struct cmd {
  int type;          //  ' ' (exec), | (pipe), '<' or '>' for redirection
};

struct execcmd {
  int type;              // ' '
  char *argv[MAXARGS];   // arguments to the command to be exec-ed
};

struct redircmd {
  int type;          // < or > 
  struct cmd *cmd;   // the command to be run (e.g., an execcmd)
  char *file;        // the input/output file
  int mode;          // the mode to open the file with
  int fd;            // the file descriptor number to use for the file
};

struct pipecmd {
  int type;          // |
  struct cmd *left;  // left side of pipe
  struct cmd *right; // right side of pipe
};

int fork1(void);  // Fork but exits on failure.
struct cmd *parsecmd(char*);

int atmpt_exec(char *buffer, const char *dirpath, char *argv[MAXARGS]){
  buffer[0] = 0;
  strcpy(buffer, dirpath);
  strcpy(buffer + strlen(dirpath), argv[0]);
  return execv(buffer, argv) < 0 ? 0 : 1;
}

void run_exec(char *argv[MAXARGS]){
  int buffer_size = strlen(argv[0]) + 6;
  char buffer[buffer_size];
  // fprintf(stderr, "EXEC: %s\n", argv[0]);

  const char * paths[] = {
    "",
    "/bin/",
    "/usr/bin/",
  };

  int succeed = 0;
  for(int i = 0; i < 3; i++){
    if(atmpt_exec(buffer, paths[i], argv)){
      succeed = 1;
      break;
    }
    // else{
    //   perror("exec");
    // }
  }
  if(!succeed){
    fprintf(stderr, "command not found: %s\n", argv[0]);
    exit(1);
  }
}

// Execute cmd.  Never returns.
void runcmd(struct cmd *cmd) {
  int p[2], r = 0;
  struct execcmd *ecmd = 0;
  struct pipecmd *pcmd = 0;
  struct redircmd *rcmd = 0;

  if(cmd == NULL)
    exit(0);
  
  switch(cmd->type){
  default:
    fprintf(stderr, "unknown runcmd\n");
    exit(-1);

  case ' ':
    ecmd = (struct execcmd*)cmd;
    if(ecmd->argv[0] == 0)
      exit(0);
    run_exec(ecmd->argv);
    break;

  case '>':
  case '<':
    rcmd = (struct redircmd*)cmd;
    int f = open(rcmd->file, rcmd->mode, 0600);
    if(f < 0){
      perror("redirect");
      break;
    }
    if(dup2(f, rcmd->fd) < 0){
      perror("redir-dup");
    }
    
    runcmd(rcmd->cmd);
    close(f);
    break;

  case '|':
    pcmd = (struct pipecmd*)cmd;
    if(pipe(p) < 0){
      perror("pipe");
      exit(1);
    }

    int fork_sts = fork1();
    if(fork_sts > 0){
      close(p[0]);
      if(dup2(p[1], 1) < 0){
        perror("pipe-parent-dup");
      }
      close(p[1]);
      runcmd(pcmd->left);
    }else if(fork_sts == 0){
      close(p[1]);
      if(dup2(p[0], 0) < 0){
        perror("pipe-child-dup");
      }
      close(p[0]);
      runcmd(pcmd->right);
    }else{
      perror("fork");
    }
    wait(&r);
    break;
  }    
  exit(0);
}

int getcmd(char *buf, int nbuf) {
  if (isatty(fileno(stdin))) fprintf(stdout, "$ ");
  memset(buf, 0, nbuf);
  return (fgets(buf, nbuf, stdin) == NULL || buf[0] == 0) ? -1 : 0;
}

int main(void) {
  // fixes gcc bug where fgets() resets file pointer and causes infinite input loop
  if (setvbuf(stdin, NULL, _IONBF, 0) != 0) {
    perror("setvbuf:");
    exit(1);
  }

  static char buf[MAXCHAR];
  int fd = 0;
  int r = 0;

  // Read and run input commands.
  while(getcmd(buf, sizeof(buf)) >= 0){
    if(strcmp(buf, "exit\n") == 0){
      exit(0);
    }

    if(buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' '){
      // Clumsy but will have to do for now.
      // Chdir has no effect on the parent if run in the child.
      buf[strlen(buf)-1] = 0;  // chop \n
      if(chdir(buf+3) < 0)
        fprintf(stderr, "cannot cd %s\n", buf+3);
      continue;
    }
    if(fork1() == 0)
      runcmd(parsecmd(buf));
    wait(&r);
  }
  exit(0);
}

int fork1(void) {
  int pid = fork();
  if(pid == -1)
    perror("fork");
  return pid;
}

struct cmd* execcmd(void) {
  struct execcmd *cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = ' ';
  return (struct cmd*)cmd;
}

struct cmd* redircmd(struct cmd *subcmd, char *file, int type) {
  struct redircmd *cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = type;
  cmd->cmd = subcmd;
  cmd->file = file;
  cmd->mode = (type == '<') ?  O_RDONLY : O_WRONLY|O_CREAT|O_TRUNC;
  cmd->fd = (type == '<') ? 0 : 1;
  return (struct cmd*)cmd;
}

struct cmd* pipecmd(struct cmd *left, struct cmd *right) {
  struct pipecmd *cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->type = '|';
  cmd->left = left;
  cmd->right = right;
  return (struct cmd*)cmd;
}

// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>\"'`";

// skips whitespace, finds token start, end (and start of next token if q / eq is specified)
 // return is either 0, a symbol or 'a' for non-symbol
int gettoken(char **ps, char *es, char **q, char **eq) {
  int ret = 0;
  
  char *s = *ps;

  // skip whitespace
  while(s < es && strchr(whitespace, *s))
    s++;
  
  // store pointer to first char of token
  if(q)
    *q = s;
  
  // get char of token
  ret = *s;
  int quote = 0;

  // check if char is a symbol
  switch(*s){

  // not more characters
  case 0:
    break;
  
  case ')':
  case ']':
  case '}':
    fprintf(stderr, "unbalanced brackets: '%c'\n", *s);
    exit(1);
    break;
  
  // first char is a symbol
  case '|':
  case '<':
  case '>':
    s++;
    break;
  // first char is a quote
  case '"':
  case '\'':
  case '`':
  // fprintf(stderr, "got quote!\n");
    if(q) (*q)++;
    do{
        s++;
        // fprintf(stderr, "scanning: '%c'\n", *s);
    }while(s < es && *s != ret);
    if(*s != ret){
      fprintf(stderr, "unbalanced quote: %c\n", ret);
      exit(1);
    }
    ret = 'a';
    quote = 1;
  break;

  // char is not a symbol
  default:
    ret = 'a';
    while(s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
      s++;
    break;
  }

  // first char after the token
  if(eq)
    *eq = s;
  
  // skip whitespace
  while(s < es && strchr(whitespace, *s))
    s++;

  if(quote) s++;
  // point to first char of second token
  *ps = s;
  return ret;
}

// skips white space then returns if one of the tokens exist
int peek_tokens(char **ps, char *es, char *toks) {
  char *s = *ps;
  while(s < es && strchr(whitespace, *s))
    s++;
  *ps = s;
  return *s && strchr(toks, *s);
}

struct cmd *parseline(char**, char*);
struct cmd *parsepipe(char**, char*);
struct cmd *parseexec(char**, char*);

// make a copy of the characters in the input buffer, starting from s through es.
// null-terminate the copy to make it a string.
char *mkcopy(char *s, char *es) {
  int n = es - s;
  char *c = malloc(n+1);
  assert(c);
  strncpy(c, s, n);
  c[n] = 0;
  return c;
}

struct cmd* parsecmd(char *s) {
  char *es = s + strlen(s);
  struct cmd *cmd = parseline(&s, es);
  peek_tokens(&s, es, "");
  if(s != es){
    fprintf(stderr, "leftovers: %s\n", s);
    exit(-1);
  }
  return cmd;
}

struct cmd* parseline(char **ps, char *es) {
  struct cmd *cmd = parsepipe(ps, es);
  return cmd;
}

struct cmd* parsepipe(char **ps, char *es) {
  struct cmd *cmd = parseexec(ps, es);
  if(peek_tokens(ps, es, "|")){
    gettoken(ps, es, 0, 0);
    cmd = pipecmd(cmd, parsepipe(ps, es));
  }
  return cmd;
}

struct cmd* parseredirs(struct cmd *cmd, char **ps, char *es) {
  char *q = 0, *eq = 0;

  while(peek_tokens(ps, es, "<>")){
    int tok = gettoken(ps, es, 0, 0);
    if(gettoken(ps, es, &q, &eq) != 'a') {
      fprintf(stderr, "missing file for redirection\n");
      exit(-1);
    }
    // fprintf(stderr, "redir token: '%.*s", (int)(eq - q), q);
    switch(tok){
    case '<':
      cmd = redircmd(cmd, mkcopy(q, eq), '<');
      break;
    case '>':
      cmd = redircmd(cmd, mkcopy(q, eq), '>');
      break;
    }
  }
  return cmd;
}

struct cmd* parseexec(char **ps, char *es) {
  char *q = 0, *eq = 0;
  int tok = 0, argc = 0;
  struct cmd *ret = execcmd();
  struct execcmd *cmd = (struct execcmd*)ret;

  ret = parseredirs(ret, ps, es);
  while(!peek_tokens(ps, es, "|")){
    if((tok=gettoken(ps, es, &q, &eq)) == 0)
      break;
    // fprintf(stderr, "exec token: '%.*s'\n", (int)(eq - q), q);
    if(tok != 'a') {
      fprintf(stderr, "syntax error\n");
      exit(-1);
    }
    cmd->argv[argc] = mkcopy(q, eq);
    argc++;
    if(argc >= MAXARGS) {
      fprintf(stderr, "too many args\n");
      exit(-1);
    }
    ret = parseredirs(ret, ps, es);
  }
  cmd->argv[argc] = 0;
  return ret;
}