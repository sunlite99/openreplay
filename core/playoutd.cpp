#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdexcept>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <poll.h>
#include <string.h>

#include "mmap_buffer.h"
#include "mjpeg_config.h"

#include "playout_ctl.h"
#include "output_adapter.h"

#include "mjpeg_frame.h"

#include "thread.h"

MmapBuffer *buffers[MAX_CHANNELS];
int marks[MAX_CHANNELS];
float play_offset;

int status_socket_fd;

int playout_source = 0;
bool did_cut = false;
bool paused = false;
bool step = false;
bool step_backward = false;
bool run_backward = false;
float playout_speed;

#define EVT_PLAYOUT_COMMAND_RECEIVED 0x00000001
class CommandReceiver : public Thread {
    public:
        CommandReceiver(EventHandler *new_dest) : dest(new_dest) {
            socket_setup( );
        }

    protected:
        void socket_setup(void) {
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(30001);
            inet_aton("127.0.0.1", &addr.sin_addr);

            socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (socket_fd == -1) {
                perror("socket");
                exit(1);
            }

            if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
                perror("bind");
            }
        }

        void run(void) {
            ssize_t ret;
            struct playout_command *cmd;
            for (;;) {
                cmd = new struct playout_command;
                ret = recvfrom(socket_fd, cmd, sizeof(struct playout_command), 0, 0, 0);
                if (ret < 0) {
                    perror("recvfrom");
                    delete cmd;
                } else if (ret < sizeof(struct playout_command)) {
                    fprintf(stderr, "received short command packet\n");
                    delete cmd;
                } else {
                    fprintf(stderr, "Got something\n");
                    dest->post_event(EVT_PLAYOUT_COMMAND_RECEIVED, cmd);
                }
            }
        }

        int socket_fd;
        EventHandler *dest;
};

class StatusSocket {
    public:
        StatusSocket( ) {
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(30002);
            inet_aton("127.0.0.1", &addr.sin_addr);

            socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (socket_fd == -1) {
                perror("socket");
                throw std::runtime_error("StatusSocket socket failed");
            }
        }

        ~StatusSocket( ) {
            close(socket_fd);
        }

        void send_status(const struct playout_status &st) {
            sendto(socket_fd, &st, sizeof(st), 0, 
                (sockaddr *)&addr, sizeof(addr));
        }
    private:
        int socket_fd;
        struct sockaddr_in addr;
};

OutputAdapter *out;

void parse_command(struct playout_command *cmd) {
    switch(cmd->cmd) {
        case PLAYOUT_CMD_CUE:
            did_cut = true;
            paused = true;
            playout_speed = cmd->new_speed;
            memcpy(marks, cmd->marks, sizeof(marks));
            play_offset = 0.0f;
            playout_source = cmd->source;
            break;

        case PLAYOUT_CMD_ADJUST_SPEED:
            playout_speed = cmd->new_speed;
            break;

        case PLAYOUT_CMD_CUT_REWIND:
            play_offset = 0.0f;
            // fall through to the cut...
        case PLAYOUT_CMD_CUT:
            playout_source = cmd->source;
            did_cut = true;
            break;
        
        case PLAYOUT_CMD_PAUSE:
            paused = true;
            break;

        case PLAYOUT_CMD_RESUME:
            paused = false;
            break;

        case PLAYOUT_CMD_STEP_FORWARD:
            step = true;
            break;

        case PLAYOUT_CMD_STEP_BACKWARD:
            step_backward = true;
            break;
    }

    fprintf(stderr, "source is now... %d\n", playout_source);
}

class Renderer {
    public:
        Renderer( ) {
            frame = (struct mjpeg_frame *) malloc(MAX_FRAME_SIZE);
            if (frame == NULL) {
                throw std::runtime_error("Renderer: failed to allocate storage");
            }
        }

        ~Renderer( ) {
            free(frame);
        }

        Picture *render_next_frame(void) {
            size_t frame_size;
            timecode_t frame_no;

            Picture *decoded;

            // Advance all streams one frame. Only decode on the one we care about.
            // (if nothing's open, this fails by design...)
            frame_size = MAX_FRAME_SIZE;                
            frame_no = marks[playout_source] + play_offset; // round to nearest whole frame

            if (buffers[playout_source]->get(frame, &frame_size, frame_no)) {
                try {
                    if (playout_speed <= 0.8 || paused) {
                        // decode and scan double a field if we can get it
                        // (should get better temporal resolution on slow motion playout)
                        if (play_offset - floorf(play_offset) < 0.5) {
                            decoded = mjpeg_decoder.decode_first_doubled(frame);
                        } else {
                            decoded = mjpeg_decoder.decode_second_doubled(frame);
                        }
                    } else {
                        decoded = mjpeg_decoder.decode_full(frame);
                    }

                    if (step) {
                        play_offset++;
                        step = false;
                    } else if (step_backward) {
                        play_offset--;
                        step_backward = false;
                    } else if (!paused) {
                        play_offset += playout_speed;
                    }

                    return decoded;
                } catch (std::runtime_error e) {
                    fprintf(stderr, "Cannot decode frame\n");
                    return NULL;
                }
            } else {
                fprintf(stderr, "off end of available video\n");
                return NULL;
            }
        }

    protected:
        struct mjpeg_frame *frame;
        MJPEGDecoder mjpeg_decoder;
};

int main(int argc, char *argv[]) {
    struct playout_command cmd;
    struct playout_status status;
    int i;

    /* generate blank picture */
    Picture *blank = Picture::alloc(720, 480, 1440, UYVY8);
    memset(blank->data, 0, 1440*480);

    Picture *current_decoded, *last_decoded = blank;

    EventHandler evtq;
    CommandReceiver recv(&evtq);
    StatusSocket statsock;
    recv.start( );

    out = new DecklinkOutput(&evtq, 0);

    Renderer r;

    // initialize buffers
    for (i = 0; i < argc - 1; ++i) {
        buffers[i] = new MmapBuffer(argv[i + 1], MAX_FRAME_SIZE);
    }

    // now, the interesting bits...
    uint32_t event;
    void *argptr;
    while (1) {
        event = evtq.wait_event(argptr);
        switch (event) {
            case EVT_PLAYOUT_COMMAND_RECEIVED:
                parse_command( (struct playout_command *) argptr );             
                break;
            case EVT_OUTPUT_NEED_FRAME:
                /* try to decode another frame */
                current_decoded = r.render_next_frame( );
                if (current_decoded != NULL) {
                    /* get rid of the old frame if we got a new one */
                    if (last_decoded != blank) {
                        Picture::free(last_decoded);
                    }

                    last_decoded = current_decoded;
                }

                /* if the decode failed just show the last frame decoded instead */
                out->SetNextFrame(last_decoded);
                break;
        }

        // (try to) send status update
        status.valid = 1;
        // timecode is always relative to stream 0
        status.timecode = marks[0] + play_offset;
        status.active_source = playout_source;

        /* 
         * note this could block the process! 
         * Remove it and see if issues go away??
         */
        statsock.send_status(status); 
    }
}
