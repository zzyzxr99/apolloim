/**
 * @file ft.h File Transfer API
 * @ingroup core
 *
 * purple
 *
 * Purple is the legal property of its developers, whose names are too numerous
 * to list here.  Please refer to the COPYRIGHT file distributed with this
 * source distribution.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * @see @ref xfer-signals
 */
#ifndef _PURPLE_FT_H_
#define _PURPLE_FT_H_

/**************************************************************************/
/** Data Structures                                                       */
/**************************************************************************/
typedef struct _PurpleXfer PurpleXfer;

#include <glib.h>
#include <stdio.h>

#include "account.h"

/**
 * Types of file transfers.
 */
typedef enum
{
	PURPLE_XFER_UNKNOWN = 0,  /**< Unknown file transfer type. */
	PURPLE_XFER_SEND,         /**< File sending.               */
	PURPLE_XFER_RECEIVE       /**< File receiving.             */

} PurpleXferType;

/**
 * The different states of the xfer.
 */
typedef enum
{
	PURPLE_XFER_STATUS_UNKNOWN = 0,   /**< Unknown, the xfer may be null. */
	PURPLE_XFER_STATUS_NOT_STARTED,   /**< It hasn't started yet. */
	PURPLE_XFER_STATUS_ACCEPTED,      /**< Receive accepted, but destination file not selected yet */
	PURPLE_XFER_STATUS_STARTED,       /**< purple_xfer_start has been called. */
	PURPLE_XFER_STATUS_DONE,          /**< The xfer completed successfully. */
	PURPLE_XFER_STATUS_CANCEL_LOCAL,  /**< The xfer was canceled by us. */
	PURPLE_XFER_STATUS_CANCEL_REMOTE  /**< The xfer was canceled by the other end, or we couldn't connect. */
} PurpleXferStatusType;

/**
 * File transfer UI operations.
 *
 * Any UI representing a file transfer must assign a filled-out
 * PurpleXferUiOps structure to the purple_xfer.
 */
typedef struct
{
	void (*new_xfer)(PurpleXfer *xfer);
	void (*destroy)(PurpleXfer *xfer);
	void (*add_xfer)(PurpleXfer *xfer);
	void (*update_progress)(PurpleXfer *xfer, double percent);
	void (*cancel_local)(PurpleXfer *xfer);
	void (*cancel_remote)(PurpleXfer *xfer);

	void (*_purple_reserved1)(void);
	void (*_purple_reserved2)(void);
	void (*_purple_reserved3)(void);
	void (*_purple_reserved4)(void);
} PurpleXferUiOps;

/**
 * A core representation of a file transfer.
 */
struct _PurpleXfer
{
	guint ref;                    /**< The reference count.                */
	PurpleXferType type;            /**< The type of transfer.               */

	PurpleAccount *account;         /**< The account.                        */

	char *who;                    /**< The person on the other end of the
	                                   transfer.                           */

	char *message;                /**< A message sent with the request     */
	char *filename;               /**< The name sent over the network.     */
	char *local_filename;         /**< The name on the local hard drive.   */
	size_t size;                  /**< The size of the file.               */

	FILE *dest_fp;                /**< The destination file pointer.       */

	char *remote_ip;              /**< The remote IP address.              */
	int local_port;               /**< The local port.                     */
	int remote_port;              /**< The remote port.                    */

	int fd;                       /**< The socket file descriptor.         */
	int watcher;                  /**< Watcher.                            */

	size_t bytes_sent;            /**< The number of bytes sent.           */
	size_t bytes_remaining;       /**< The number of bytes remaining.      */
	time_t start_time;            /**< When the transfer of data began.    */
	time_t end_time;              /**< When the transfer of data ended.    */

	size_t current_buffer_size;   /**< This gradually increases for fast
	                                   network connections. */

	PurpleXferStatusType status;    /**< File Transfer's status.             */

	/* I/O operations. */
	struct
	{
		void (*init)(PurpleXfer *xfer);
		void (*request_denied)(PurpleXfer *xfer);
		void (*start)(PurpleXfer *xfer);
		void (*end)(PurpleXfer *xfer);
		void (*cancel_send)(PurpleXfer *xfer);
		void (*cancel_recv)(PurpleXfer *xfer);
		gssize (*read)(guchar **buffer, PurpleXfer *xfer);
		gssize (*write)(const guchar *buffer, size_t size, PurpleXfer *xfer);
		void (*ack)(PurpleXfer *xfer, const guchar *buffer, size_t size);

	} ops;

	PurpleXferUiOps *ui_ops;            /**< UI-specific operations. */
	void *ui_data;                    /**< UI-specific data.       */

	void *data;                       /**< prpl-specific data.     */
};

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************/
/** @name File Transfer API                                               */
/**************************************************************************/
/*@{*/

/**
 * Creates a new file transfer handle.
 * This is called by prpls.
 * The handle starts with a ref count of 1, and this reference
 * is owned by the core. The prpl normally does not need to
 * purple_xfer_ref or unref.
 *
 * @param account The account sending or receiving the file.
 * @param type    The type of file transfer.
 * @param who     The name of the remote user.
 *
 * @return A file transfer handle.
 */
PurpleXfer *purple_xfer_new(PurpleAccount *account,
								PurpleXferType type, const char *who);

/**
 * Returns all xfers
 *
 * @return all current xfers with refs
 */
GList *purple_xfers_get_all(void);

/**
 * Increases the reference count on a PurpleXfer.
 * Please call purple_xfer_unref later.
 *
 * @param xfer A file transfer handle.
 */
void purple_xfer_ref(PurpleXfer *xfer);

/**
 * Decreases the reference count on a PurpleXfer.
 * If the reference reaches 0, purple_xfer_destroy (an internal function)
 * will destroy the xfer. It calls the ui destroy cb first.
 * Since the core keeps a ref on the xfer, only an erroneous call to
 * this function will destroy the xfer while still in use.
 *
 * @param xfer A file transfer handle.
 */
void purple_xfer_unref(PurpleXfer *xfer);

/**
 * Requests confirmation for a file transfer from the user. If receiving
 * a file which is known at this point, this requests user to accept and
 * save the file. If the filename is unknown (not set) this only requests user
 * to accept the file transfer. In this case protocol must call this function
 * again once the filename is available.
 *
 * @param xfer The file transfer to request confirmation on.
 */
void purple_xfer_request(PurpleXfer *xfer);

/**
 * Called if the user accepts the file transfer request.
 *
 * @param xfer     The file transfer.
 * @param filename The filename.
 */
void purple_xfer_request_accepted(PurpleXfer *xfer, const char *filename);

/**
 * Called if the user rejects the file transfer request.
 *
 * @param xfer The file transfer.
 */
void purple_xfer_request_denied(PurpleXfer *xfer);

/**
 * Returns the type of file transfer.
 *
 * @param xfer The file transfer.
 *
 * @return The type of the file transfer.
 */
PurpleXferType purple_xfer_get_type(const PurpleXfer *xfer);

/**
 * Returns the account the file transfer is using.
 *
 * @param xfer The file transfer.
 *
 * @return The account.
 */
PurpleAccount *purple_xfer_get_account(const PurpleXfer *xfer);

/**
 * Returns the name of the remote user.
 *
 * @param xfer The file transfer.
 *
 * @return The name of the remote user.
 */
const char *purple_xfer_get_remote_user(const PurpleXfer *xfer);

/**
 * Returns the status of the xfer.
 *
 * @param xfer The file transfer.
 *
 * @return The status.
 */
PurpleXferStatusType purple_xfer_get_status(const PurpleXfer *xfer);

/**
 * Returns true if the file transfer was canceled.
 *
 * @param xfer The file transfer.
 *
 * @return Whether or not the transfer was canceled.
 */
gboolean purple_xfer_is_canceled(const PurpleXfer *xfer);

/**
 * Returns the completed state for a file transfer.
 *
 * @param xfer The file transfer.
 *
 * @return The completed state.
 */
gboolean purple_xfer_is_completed(const PurpleXfer *xfer);

/**
 * Returns the name of the file being sent or received.
 *
 * @param xfer The file transfer.
 *
 * @return The filename.
 */
const char *purple_xfer_get_filename(const PurpleXfer *xfer);

/**
 * Returns the file's destination filename,
 *
 * @param xfer The file transfer.
 *
 * @return The destination filename.
 */
const char *purple_xfer_get_local_filename(const PurpleXfer *xfer);

/**
 * Returns the number of bytes sent (or received) so far.
 *
 * @param xfer The file transfer.
 *
 * @return The number of bytes sent.
 */
size_t purple_xfer_get_bytes_sent(const PurpleXfer *xfer);

/**
 * Returns the number of bytes remaining to send or receive.
 *
 * @param xfer The file transfer.
 *
 * @return The number of bytes remaining.
 */
size_t purple_xfer_get_bytes_remaining(const PurpleXfer *xfer);

/**
 * Returns the size of the file being sent or received.
 *
 * @param xfer The file transfer.
 *
 * @return The total size of the file.
 */
size_t purple_xfer_get_size(const PurpleXfer *xfer);

/**
 * Returns the current percentage of progress of the transfer.
 *
 * This is a number between 0 (0%) and 1 (100%).
 *
 * @param xfer The file transfer.
 *
 * @return The percentage complete.
 */
double purple_xfer_get_progress(const PurpleXfer *xfer);

/**
 * Returns the local port number in the file transfer.
 *
 * @param xfer The file transfer.
 *
 * @return The port number on this end.
 */
unsigned int purple_xfer_get_local_port(const PurpleXfer *xfer);

/**
 * Returns the remote IP address in the file transfer.
 *
 * @param xfer The file transfer.
 *
 * @return The IP address on the other end.
 */
const char *purple_xfer_get_remote_ip(const PurpleXfer *xfer);

/**
 * Returns the remote port number in the file transfer.
 *
 * @param xfer The file transfer.
 *
 * @return The port number on the other end.
 */
unsigned int purple_xfer_get_remote_port(const PurpleXfer *xfer);

/**
 * Sets the completed state for the file transfer.
 *
 * @param xfer      The file transfer.
 * @param completed The completed state.
 */
void purple_xfer_set_completed(PurpleXfer *xfer, gboolean completed);

/**
 * Sets the filename for the file transfer.
 *
 * @param xfer     The file transfer.
 * @param message The message.
 */
void purple_xfer_set_message(PurpleXfer *xfer, const char *message);

/**
 * Sets the filename for the file transfer.
 *
 * @param xfer     The file transfer.
 * @param filename The filename.
 */
void purple_xfer_set_filename(PurpleXfer *xfer, const char *filename);

/**
 * Sets the local filename for the file transfer.
 *
 * @param xfer     The file transfer.
 * @param filename The filename
 */
void purple_xfer_set_local_filename(PurpleXfer *xfer, const char *filename);

/**
 * Sets the size of the file in a file transfer.
 *
 * @param xfer The file transfer.
 * @param size The size of the file.
 */
void purple_xfer_set_size(PurpleXfer *xfer, size_t size);

/**
 * Sets the current working position in the active file transfer.  This
 * can be used to jump backward in the file if the protocol detects
 * that some bit of data needs to be resent or has been sent twice.
 *
 * It's used for pausing and resuming an oscar file transfer.
 *
 * @param xfer       The file transfer.
 * @param bytes_sent The new current position in the file.  If we're
 *                   sending a file then this is the byte that we will
 *                   send.  If we're receiving a file, this is the
 *                   next byte that we expect to receive.
 */
void purple_xfer_set_bytes_sent(PurpleXfer *xfer, size_t bytes_sent);

/**
 * Returns the UI operations structure for a file transfer.
 *
 * @param xfer The file transfer.
 *
 * @return The UI operations structure.
 */
PurpleXferUiOps *purple_xfer_get_ui_ops(const PurpleXfer *xfer);

/**
 * Sets the read function for the file transfer.
 *
 * @param xfer The file transfer.
 * @param fnc  The read function.
 */
void purple_xfer_set_read_fnc(PurpleXfer *xfer,
		gssize (*fnc)(guchar **, PurpleXfer *));

/**
 * Sets the write function for the file transfer.
 *
 * @param xfer The file transfer.
 * @param fnc  The write function.
 */
void purple_xfer_set_write_fnc(PurpleXfer *xfer,
		gssize (*fnc)(const guchar *, size_t, PurpleXfer *));

/**
 * Sets the acknowledge function for the file transfer.
 *
 * @param xfer The file transfer.
 * @param fnc  The acknowledge function.
 */
void purple_xfer_set_ack_fnc(PurpleXfer *xfer,
		void (*fnc)(PurpleXfer *, const guchar *, size_t));

/**
 * Sets the function to be called if the request is denied.
 *
 * @param xfer The file transfer.
 * @param fnc The request denied prpl callback.
 */
void purple_xfer_set_request_denied_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *));

/**
 * Sets the transfer initialization function for the file transfer.
 *
 * This function is required, and must call purple_xfer_start() with
 * the necessary parameters. This will be called if the file transfer
 * is accepted by the user.
 *
 * @param xfer The file transfer.
 * @param fnc  The transfer initialization function.
 */
void purple_xfer_set_init_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *));

/**
 * Sets the start transfer function for the file transfer.
 *
 * @param xfer The file transfer.
 * @param fnc  The start transfer function.
 */
void purple_xfer_set_start_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *));

/**
 * Sets the end transfer function for the file transfer.
 *
 * @param xfer The file transfer.
 * @param fnc  The end transfer function.
 */
void purple_xfer_set_end_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *));

/**
 * Sets the cancel send function for the file transfer.
 *
 * @param xfer The file transfer.
 * @param fnc  The cancel send function.
 */
void purple_xfer_set_cancel_send_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *));

/**
 * Sets the cancel receive function for the file transfer.
 *
 * @param xfer The file transfer.
 * @param fnc  The cancel receive function.
 */
void purple_xfer_set_cancel_recv_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *));

/**
 * Reads in data from a file transfer stream.
 *
 * @param xfer   The file transfer.
 * @param buffer The buffer that will be created to contain the data.
 *
 * @return The number of bytes read, or -1.
 */
gssize purple_xfer_read(PurpleXfer *xfer, guchar **buffer);

/**
 * Writes data to a file transfer stream.
 *
 * @param xfer   The file transfer.
 * @param buffer The buffer to read the data from.
 * @param size   The number of bytes to write.
 *
 * @return The number of bytes written, or -1.
 */
gssize purple_xfer_write(PurpleXfer *xfer, const guchar *buffer, gsize size);

/**
 * Starts a file transfer.
 *
 * Either @a fd must be specified <i>or</i> @a ip and @a port on a
 * file receive transfer. On send, @a fd must be specified, and
 * @a ip and @a port are ignored.
 *
 * @param xfer The file transfer.
 * @param fd   The file descriptor for the socket.
 * @param ip   The IP address to connect to.
 * @param port The port to connect to.
 */
void purple_xfer_start(PurpleXfer *xfer, int fd, const char *ip,
					 unsigned int port);

/**
 * Ends a file transfer.
 *
 * @param xfer The file transfer.
 */
void purple_xfer_end(PurpleXfer *xfer);

/**
 * Adds a new file transfer to the list of file transfers. Call this only
 * if you are not using purple_xfer_start.
 *
 * @param xfer The file transfer.
 */
void purple_xfer_add(PurpleXfer *xfer);

/**
 * Cancels a file transfer on the local end.
 *
 * @param xfer The file transfer.
 */
void purple_xfer_cancel_local(PurpleXfer *xfer);

/**
 * Cancels a file transfer from the remote end.
 *
 * @param xfer The file transfer.
 */
void purple_xfer_cancel_remote(PurpleXfer *xfer);

/**
 * Displays a file transfer-related error message.
 *
 * This is a wrapper around purple_notify_error(), which automatically
 * specifies a title ("File transfer to <i>user</i> failed" or
 * "File Transfer from <i>user</i> failed").
 *
 * @param type    The type of file transfer.
 * @param account The account sending or receiving the file.
 * @param who     The user on the other end of the transfer.
 * @param msg     The message to display.
 */
void purple_xfer_error(PurpleXferType type, PurpleAccount *account, const char *who, const char *msg);

/**
 * Updates file transfer progress.
 *
 * @param xfer      The file transfer.
 */
void purple_xfer_update_progress(PurpleXfer *xfer);

/**
 * Displays a file transfer-related message in the conversation window
 *
 * This is a wrapper around purple_conversation_write
 *
 * @param xfer The file transfer to which this message relates.
 * @param message The message to display.
 * @param is_error Is this an error message?.
 */
void purple_xfer_conversation_write(PurpleXfer *xfer, char *message, gboolean is_error);

/*@}*/

/**************************************************************************/
/** @name UI Registration Functions                                       */
/**************************************************************************/
/*@{*/

/**
 * Returns the handle to the file transfer subsystem
 *
 * @return The handle
 */
void *purple_xfers_get_handle(void);

/**
 * Initializes the file transfer subsystem
 */
void purple_xfers_init(void);

/**
 * Uninitializes the file transfer subsystem
 */
void purple_xfers_uninit(void);

/**
 * Sets the UI operations structure to be used in all purple file transfers.
 *
 * @param ops The UI operations structure.
 */
void purple_xfers_set_ui_ops(PurpleXferUiOps *ops);

/**
 * Returns the UI operations structure to be used in all purple file transfers.
 *
 * @return The UI operations structure.
 */
PurpleXferUiOps *purple_xfers_get_ui_ops(void);

/*@}*/

#ifdef __cplusplus
}
#endif

#endif /* _PURPLE_FT_H_ */
