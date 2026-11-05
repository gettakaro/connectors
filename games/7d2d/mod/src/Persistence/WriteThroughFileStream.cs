using System;
using System.IO;

namespace Takaro.Persistence
{
    /// <summary>
    /// A file stream that hands every write to the operating system before it returns.
    /// LiteDB 5 writes each page with one PAGE_SIZE (8192 byte) Write call and reads the log
    /// back through a second stream during a checkpoint. Mono's FileStream only bypasses its
    /// buffer for writes larger than the buffer, so with LiteDB's own 8192-byte buffer the last
    /// page of every commit stayed in the writer's memory, the reader found the file one page
    /// short, and the checkpoint failed with "ReadFull must read PAGE_SIZE bytes". The
    /// database is therefore opened on two of these streams instead of a filename: LiteDB then
    /// shares one stream per file between its reader and writer, and nothing it wrote can be
    /// missing from the file, also when the process is killed right after a commit.
    /// </summary>
    public sealed class WriteThroughFileStream : Stream
    {
        private readonly FileStream _file;

        public WriteThroughFileStream(string path)
        {
            // FileShare.Read: a second server on the same save can read but never write it.
            _file = new FileStream(
                path,
                FileMode.OpenOrCreate,
                FileAccess.ReadWrite,
                FileShare.Read,
                4096,
                FileOptions.RandomAccess
            );
        }

        public string Name => _file.Name;

        public override bool CanRead => true;
        public override bool CanSeek => true;
        public override bool CanWrite => true;
        public override long Length => _file.Length;

        public override long Position
        {
            get => _file.Position;
            set => _file.Position = value;
        }

        // LiteDB calls Flush where it wants the bytes on disk (checkpoint, header).
        public override void Flush() => _file.Flush(true);

        public override int Read(byte[] buffer, int offset, int count) =>
            _file.Read(buffer, offset, count);

        public override long Seek(long offset, SeekOrigin origin) => _file.Seek(offset, origin);

        public override void SetLength(long value)
        {
            _file.SetLength(value);
            _file.Flush();
        }

        public override void Write(byte[] buffer, int offset, int count)
        {
            _file.Write(buffer, offset, count);
            _file.Flush();
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
                _file.Dispose();
            base.Dispose(disposing);
        }
    }
}
