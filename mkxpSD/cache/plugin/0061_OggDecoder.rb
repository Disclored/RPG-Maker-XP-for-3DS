#==============================================================================#
#                              Ogg Decoder                                     #
#                                by Marin                                      #
#==============================================================================#
#  Made with the purpose of reading Ogg Vorbis comments within the metadata,   #
#      and as a result has optimisation built in only for this purpose.        #
#                                                                              #
#   To read all the data within an Ogg file, ensure `metadata_only` is set to  #
#           false when reading initializing the OggDecoder object.             #
#==============================================================================#
#                       Credit is always appreciated                           #
#==============================================================================#

class ByteReader
  def initialize(filename, endianness = :big)
    @file = File.new(filename, "rb")
    @endianness = endianness
  end

  def read(n = 1)
    return read_char(1).bytes[0] if n == 1
    return read_char(n).bytes
  end

  def read_char(n = 1)
    return @file.read(n)
  end

  def read_int
    return ByteReader.bytes_to_int(read(4), @endianness)
  end

  def seek(pos)
    @file.pos = pos
  end

  def eof?
    @file.eof?
  end

  def close
    @file.close
  end

  def self.bytes_to_int(bytes, endianness = :big)
    if endianness == :big
      return bytes[0] + (bytes[1] << 8) + (bytes[2] << 16) + (bytes[3] << 24)
    elsif endianness == :little
      return bytes[3] + (bytes[2] << 8) + (bytes[1] << 16) + (bytes[0] << 24)
    end
  end
end

class OggDecoder
  attr_reader :pages
  attr_reader :packets
  attr_reader :id_header
  attr_reader :comment_header

  def initialize(filename, metadata_only = false)
    @reader = ByteReader.new(filename)
    @pages = []
    packet_index = 1
    while !@reader.eof?
      if @pages[-1] && @pages[-1].header_type == 4
        raise DecodeException.new "End-Of-File marker was read but the file is not fully read yet."
      end
      page = Page.new(@reader, @pages.size == 0, metadata_only, packet_index)
      @pages << page
      # Ensure we quit loading more pages if the current page already has the 2nd header packet
      # The specification was unclear whether the comment packet can also occur in the first page,
      # or whether it's always the first packet of the second page, so I made sure to handle both cases.
      # It appearing in the first page is untested, however.
      if metadata_only
        if @pages.size == 1
          if page.packets.size > 1
            break
          end
        elsif @pages.size == 2
          break
        end
      end
      packet_index -= page.packets.size
    end
    @packets = @pages.flat_map { |page| page.packets }
    if @packets.size < 2
      raise DecodeException.new "Expected at least 2 packets"
    end
    @id_header = @packets.shift.data
    @comment_header = Vorbis::Comments.new(@packets.shift.data)
    @reader.close
  rescue => e
    raise e if e.is_a?(DecodeException)
    raise DecodeException.new "Invalid file format or data"
  end

  class Page
    attr_reader :capture_pattern
    attr_reader :version
    attr_reader :header_type
    attr_reader :granule_position
    attr_reader :bitstream_serial_number
    attr_reader :page_sequence_number
    attr_reader :checksum
    attr_reader :segment_count
    attr_reader :segment_sizes
    attr_reader :segments
    attr_reader :packets

    def initialize(reader, first, metadata_only, packet_index)
      @reader = reader
      @capture_pattern = @reader.read_char(4)
      raise DecodeException.new "Page does not begin with OggS." if @capture_pattern != "OggS"
      @version = @reader.read
      raise DecodeException.new "Ogg version not set to 0." if @version != 0
      @header_type = @reader.read
      raise DecodeException.new "Expected header type to be 2, but is #{@header_type}." if first && @header_type != 2
      @granule_position = @reader.read(8)
      @bitstream_serial_number = @reader.read_int
      @page_sequence_number = @reader.read_int
      @checksum = @reader.read_int
      @segment_count = @reader.read
      @segment_sizes = []
      @segment_count.times do
        @segment_sizes << @reader.read
      end
      @segments = []
      @packets = []
      last_size = -1
      @segment_sizes.each do |size|
        segment = Segment.new(@reader, size)
        @segments << segment
        if last_size == 255 && @packets.size > 0
          @packets[-1].data.concat(segment.data)
        else
          break if metadata_only && @packets[-1] && @packets[-1].data[0] == 3 # Packet Type == 3
          @packets << Packet.new(segment.data)
        end
        last_size = size
      end
    end
  end

  class Segment
    attr_reader :size
    attr_reader :data

    def initialize(reader, size)
      @reader = reader
      @size = size
      @data = reader.read(@size)
      @data = [@data] if !@data.is_a?(Array)
    end
  end

  class Packet
    attr_reader :data

    def initialize(data)
      @data = data
    end
  end

  class DecodeException < StandardError
    def initialize(msg = nil)
      super("Error while decoding Ogg file." + (msg.nil? ? "" : "\n" + msg))
    end
  end
end

class Vorbis
  class Comments
    attr_reader :packet_type
    attr_reader :codec
    attr_reader :vendor
    attr_reader :comments

    def initialize(data)
      @data = data
      @packet_type = @data.shift
      @codec = @data.shift(6).pack("c*")
      vendor_length = ByteReader.bytes_to_int(@data.shift(4), :big)
      @vendor = @data.shift(vendor_length).pack("c*")
      comment_count = ByteReader.bytes_to_int(@data.shift(4), :big)
      @comments = []
      comment_count.times do
        length = ByteReader.bytes_to_int(@data.shift(4), :big)
        string = @data.shift(length).pack("c*")
        key, value = string.split("=")
        @comments << [key.upcase, value]
      end
    end

    def has?(key)
      return @comments.any? { |e| e[0] == key.upcase }
    end

    def get(key)
      return @comments.select { |e| e[0] == key.upcase }.map { |e| e[1] }
    end

    def get_one(key)
      return get(key)[0]
    end
  end
end