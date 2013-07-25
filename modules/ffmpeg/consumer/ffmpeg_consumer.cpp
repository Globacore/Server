#include "../StdAfx.h"

#include "ffmpeg_consumer.h"

#include "../util/error.h"

#include <common/exception/win32_exception.h>
#include <common/utility/assert.h>
#include <common/utility/string.h>
#include <common/concurrency/future_util.h>
#include <common/env.h>
#include <common/scope_exit.h>

#include <core/parameters/parameters.h>
#include <core/consumer/frame_consumer.h>
#include <core/mixer/read_frame.h>
#include <core/producer/frame/pixel_format.h>
#include <core/video_format.h>

#include <boost/noncopyable.hpp>
#include <boost/rational.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <tbb/atomic.h>
#include <tbb/concurrent_queue.h>

#include <agents.h>

#pragma warning(push)
#pragma warning(disable: 4244)

extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavformat/avformat.h>
	#include <libavcodec/avcodec.h>
	#include <libavutil/avutil.h>
	#include <libavutil/frame.h>
	#include <libavutil/opt.h>
	#include <libavutil/imgutils.h>
	#include <libavfilter/avfilter.h>
	#include <libavfilter/buffersink.h>
	#include <libavfilter/buffersrc.h>
}

#pragma warning(pop)

using namespace Concurrency;

namespace caspar { namespace ffmpeg {
	
class ffmpeg_consumer sealed : public core::frame_consumer
{
public:
	// Static Members
	
	struct configuration
	{
		std::string				filename;
		std::string				options;

		configuration()
		{
		}
	};
	
private:

	boost::filesystem::path			 path_;
									 
	core::video_format_desc			 in_video_format_;
									 
	configuration					 configuration_;
									 
	std::shared_ptr<AVFormatContext> oc_;
	tbb::atomic<bool>				 abort_request_;
									 
	std::shared_ptr<AVStream>		 video_st_;
	std::shared_ptr<AVStream>		 audio_st_;

	std::int64_t					 counter_;
									 										
	std::int64_t					 filter_in_rescale_delta_last_;	

    AVFilterContext*				 audio_graph_in_;  
    AVFilterContext*				 audio_graph_out_; 
    std::shared_ptr<AVFilterGraph>	 audio_graph_;           

    AVFilterContext*				 video_graph_in_;  
    AVFilterContext*				 video_graph_out_; 
    std::shared_ptr<AVFilterGraph>	 video_graph_;  

public:

	ffmpeg_consumer(const configuration& configuration)
		: configuration_(configuration)
		, path_(configuration.filename)
		, filter_in_rescale_delta_last_(AV_NOPTS_VALUE)
		, counter_(0)
	{		
		abort_request_ = false;	
	}
		
	~ffmpeg_consumer()
	{
		if(oc_)
		{
			encode_video(nullptr);
			encode_audio(nullptr);								
			write_packet(nullptr);
					
			FF(av_write_trailer(oc_.get()));
		
			video_st_.reset();
			audio_st_.reset();
				
			if (!(oc_->oformat->flags & AVFMT_NOFILE))
				avio_close(oc_->pb);

			oc_.reset();
		}
	}
		
	void initialize(const core::video_format_desc& format_desc, int channel_index) override 
	{
		try
		{				
			static boost::regex prot_exp("^.+:.*" );

			if(!boost::regex_match(path_.string(), prot_exp))
			{
				if(!path_.is_complete())
					path_ = narrow(env::media_folder()) + path_.string();
			
				if(boost::filesystem::exists(path_))
					BOOST_THROW_EXCEPTION(invalid_argument() << msg_info("File exists"));
			}
							
			const auto oformat_name = get_option<std::string>("(f|format)");

			AVFormatContext* oc;
			FF(avformat_alloc_output_context2(&oc, nullptr, oformat_name ? oformat_name->c_str() : nullptr, path_.string().c_str()));
			oc_.reset(oc, avformat_free_context);
		
			CASPAR_VERIFY(oc_->oformat);

			oc_->interrupt_callback.callback = ffmpeg_consumer::interrupt_cb;
			oc_->interrupt_callback.opaque   = this;	

			CASPAR_VERIFY(format_desc.format != core::video_format::invalid);

			in_video_format_ = format_desc;
							
			CASPAR_VERIFY(oc_->oformat);
			
			const auto video_codec_name = get_option<std::string>("(c:v|vcodec)");

			const auto video_codec = video_codec_name 
									? avcodec_find_encoder_by_name(video_codec_name->c_str())
									: avcodec_find_encoder(oc_->oformat->video_codec);
						
			const auto audio_codec_name = get_option<std::string>("(c:a|acodec)");
			
			const auto audio_codec = audio_codec_name 
									? avcodec_find_encoder_by_name(audio_codec_name->c_str())
									: avcodec_find_encoder(oc_->oformat->audio_codec);
			
			CASPAR_VERIFY(video_codec);
			CASPAR_VERIFY(audio_codec);

			configure_video_filters(*video_codec);
			configure_audio_filters(*audio_codec);

			video_st_ = open_encoder(*video_codec);	
			audio_st_ = open_encoder(*audio_codec);
			
			av_dump_format(oc_.get(), 0, path_.string().c_str(), 1);
		
			if (!(oc_->oformat->flags & AVFMT_NOFILE)) 
				FF(avio_open(&oc_->pb, path_.string().c_str(), AVIO_FLAG_WRITE));

			FF(avformat_write_header(oc_.get(), NULL));
		}
		catch(...)
		{
			video_st_.reset();
			audio_st_.reset();
			oc_.reset();
			throw;
		}
	}

	boost::unique_future<bool> send(const safe_ptr<core::read_frame>& frame) override
	{		
		CASPAR_VERIFY(in_video_format_.format != core::video_format::invalid);
				
		encode_video(frame);		
		encode_audio(frame);
				
		return make_ready_future(true);
	}

	std::wstring print() const override
	{
		return L"ffmpeg_consumer[" + widen(configuration_.filename) + L"]";
	}
	
	virtual boost::property_tree::wptree info() const override
	{
		return boost::property_tree::wptree();
	}

	bool has_synchronization_clock() const override
	{
		return false;
	}

	size_t buffer_depth() const override
	{
		return 0;
	}

	int index() const override
	{
		return 200;
	}

	int64_t presentation_frame_age_millis() const override
	{
		return 0;
	}

private:

	static int interrupt_cb(void* ctx)
	{
		CASPAR_ASSERT(ctx);
		return reinterpret_cast<ffmpeg_consumer*>(ctx)->abort_request_;		
	}
		
	std::shared_ptr<AVStream> open_encoder(const AVCodec& codec)
	{			
		auto st = avformat_new_stream(oc_.get(), &codec);
		if (!st) 		
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info("Could not allocate video-stream.") << boost::errinfo_api_function("av_new_stream"));	

		auto enc = st->codec;
				
		CASPAR_VERIFY(enc);
						
		switch(enc->codec_type)
		{
			case AVMEDIA_TYPE_VIDEO:
			{
				enc->time_base			  = video_graph_out_->inputs[0]->time_base;
				enc->pix_fmt			  = static_cast<AVPixelFormat>(video_graph_out_->inputs[0]->format);
				enc->sample_aspect_ratio  = video_graph_out_->inputs[0]->sample_aspect_ratio;
				enc->width				  = video_graph_out_->inputs[0]->w;
				enc->height				  = video_graph_out_->inputs[0]->h;
				enc->gop_size			  = get_option("g", enc->gop_size);
			
				break;
			}
			case AVMEDIA_TYPE_AUDIO:
			{
				enc->time_base			  = audio_graph_out_->inputs[0]->time_base;
				enc->sample_fmt			  = static_cast<AVSampleFormat>(audio_graph_out_->inputs[0]->format);
				enc->sample_rate		  = audio_graph_out_->inputs[0]->sample_rate;
				enc->channel_layout		  = audio_graph_out_->inputs[0]->channel_layout;
				enc->channels			  = audio_graph_out_->inputs[0]->channels;
				enc->bit_rate			  = 192000;
			
				break;
			}
		}
								
		if(oc_->oformat->flags & AVFMT_GLOBALHEADER)
			enc->flags |= CODEC_FLAG_GLOBAL_HEADER;
		
		static const std::array<std::string, 4> suffix_map = {{"v", "a", "d", "s"}};
						
		AVDictionary* codec_opts = nullptr;

		parse_options(&codec_opts, "-(?<NAME>[^:\\s]+)\\s(?<VALUE>[^-\\s]+)");
		parse_options(&codec_opts, "(?<NAME>[^:\\s]+):" + suffix_map.at(enc->codec_type) + "\\s(?<VALUE>[^-\\s]+)");
				
        if (!av_dict_get(codec_opts, "threads", NULL, 0))
            av_dict_set(&codec_opts, "threads", "auto", 0);
		
		FF(avcodec_open2(enc, &codec, codec_opts ? &codec_opts : nullptr));		
				
		if(enc->codec_type == AVMEDIA_TYPE_AUDIO && !(codec.capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE))
		{
			CASPAR_ASSERT(enc->frame_size > 0);
			av_buffersink_set_frame_size(audio_graph_out_, enc->frame_size);
		}

		av_dict_free(&codec_opts);

		return std::shared_ptr<AVStream>(st, [this](AVStream* st)
		{
			avcodec_close(st->codec);
		});
	}

	void configure_audio_filters(const AVCodec& codec)
	{
		audio_graph_.reset(avfilter_graph_alloc(), [](AVFilterGraph* p)
		{
			avfilter_graph_free(&p);
		});
		
		const auto asrc_args = (boost::format("sample_rate=%1%:sample_fmt=%2%:channels=%3%:time_base=%4%/%5%")
			% in_video_format_.audio_sample_rate
			% av_get_sample_fmt_name(AV_SAMPLE_FMT_S32)
			% 2 // TODO:
			% 1	% in_video_format_.audio_sample_rate).str();
				
		// TODO:
		//if (is->audio_filter_src.channel_layout)
		//	snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
		//			 ":channel_layout=0x%"PRIx64,  is->audio_filter_src.channel_layout);
				
		AVFilterContext* filt_asrc = nullptr;
		FF(avfilter_graph_create_filter(&filt_asrc,
										avfilter_get_by_name("abuffer"), 
										"ffmpeg_consumer_abuffer",
										asrc_args.c_str(), 
										nullptr, 
										audio_graph_.get()));
				
		AVFilterContext* filt_asink = nullptr;
		FF(avfilter_graph_create_filter(&filt_asink,
										avfilter_get_by_name("abuffersink"), 
										"ffmpeg_consumer_abuffersink",
										NULL, NULL, 
										audio_graph_.get()));
		
#pragma warning (disable : 4245)
		FF(av_opt_set_int(filt_asink,	   "all_channel_counts", 1,	AV_OPT_SEARCH_CHILDREN));
		FF(av_opt_set_int_list(filt_asink, "sample_fmts",		 codec.sample_fmts,				-1, AV_OPT_SEARCH_CHILDREN));
		FF(av_opt_set_int_list(filt_asink, "channel_layouts",	 codec.channel_layouts,			-1, AV_OPT_SEARCH_CHILDREN));
		FF(av_opt_set_int_list(filt_asink, "sample_rates"   ,	 codec.supported_samplerates,	-1, AV_OPT_SEARCH_CHILDREN));
#pragma warning (default : 4245)
			
		configure_filtergraph(*audio_graph_, get_option<std::string>("af"), *filt_asrc, *filt_asink);

		audio_graph_in_  = filt_asrc;
		audio_graph_out_ = filt_asink;
	}

	void configure_video_filters(const AVCodec& codec)
	{
		video_graph_.reset(avfilter_graph_alloc(), [](AVFilterGraph* p)
		{
			avfilter_graph_free(&p);
		});
		
		const auto vsrc_args = (boost::format("video_size=%1%x%2%:pix_fmt=%3%:time_base=%4%/%5%:pixel_aspect=%6%/%7%:frame_rate=%8%/%9%")
			% in_video_format_.width % in_video_format_.height
			% AV_PIX_FMT_BGRA
			% in_video_format_.duration	% in_video_format_.time_scale
			% 0 % 1 // TODO:
			% in_video_format_.time_scale % in_video_format_.duration).str();
					
		AVFilterContext* filt_vsrc = nullptr;			
		FF(avfilter_graph_create_filter(&filt_vsrc,
										avfilter_get_by_name("buffer"), 
										"ffmpeg_consumer_buffer",
										vsrc_args.c_str(), 
										nullptr, 
										video_graph_.get()));
				
		AVFilterContext* filt_vsink = nullptr;
		FF(avfilter_graph_create_filter(&filt_vsink,
										avfilter_get_by_name("buffersink"), 
										"ffmpeg_consumer_buffersink",
										NULL, NULL, 
										video_graph_.get()));
		
#pragma warning (disable : 4245)
		FF(av_opt_set_int_list(filt_vsink, "pix_fmts", codec.pix_fmts, -1, AV_OPT_SEARCH_CHILDREN));
#pragma warning (default : 4245)
			
		configure_filtergraph(*video_graph_, get_option<std::string>("vf"), *filt_vsrc, *filt_vsink);

		video_graph_in_  = filt_vsrc;
		video_graph_out_ = filt_vsink;
	}

	void configure_filtergraph(AVFilterGraph& graph, boost::optional<std::string> filtergraph, AVFilterContext& source_ctx, AVFilterContext& sink_ctx)
	{
		AVFilterInOut* outputs = nullptr;
		AVFilterInOut* inputs = nullptr;

		try
		{
			if(filtergraph) 
			{
				outputs = avfilter_inout_alloc();
				inputs  = avfilter_inout_alloc();

				CASPAR_VERIFY(outputs && inputs);

				outputs->name       = av_strdup("in");
				outputs->filter_ctx = &source_ctx;
				outputs->pad_idx    = 0;
				outputs->next       = nullptr;

				inputs->name        = av_strdup("out");
				inputs->filter_ctx  = &sink_ctx;
				inputs->pad_idx     = 0;
				inputs->next        = nullptr;

				FF(avfilter_graph_parse(&graph, filtergraph->c_str(), &inputs, &outputs, nullptr));
			} 
			else 
			{
				FF(avfilter_link(&source_ctx, 0, &sink_ctx, 0));
			}

			FF(avfilter_graph_config(&graph, nullptr));
		}
		catch(...)
		{
			avfilter_inout_free(&outputs);
			avfilter_inout_free(&inputs);
			throw;
		}
	}
	
	void encode_video(const std::shared_ptr<core::read_frame>& frame_ptr)
	{		
		if(!video_st_)
			return;
		
		auto enc = video_st_->codec;
			
		if(frame_ptr)
		{
			std::shared_ptr<AVFrame> src_av_frame(av_frame_alloc(), [frame_ptr](AVFrame* frame)
			{
				av_frame_free(&frame);
			});
			avcodec_get_frame_defaults(src_av_frame.get());		

			src_av_frame->format = AV_PIX_FMT_BGRA;
			src_av_frame->width  = in_video_format_.width;
			src_av_frame->height = in_video_format_.height;
			src_av_frame->pts	 = counter_++;

			FF(av_image_fill_arrays(src_av_frame->data,
									src_av_frame->linesize,
									frame_ptr->image_data().begin(),
									static_cast<AVPixelFormat>(src_av_frame->format), 
									in_video_format_.width, 
									in_video_format_.height, 
									1));

			FF(av_buffersrc_add_frame(video_graph_in_, src_av_frame.get()));
		}		

		while(true)
		{
			std::shared_ptr<AVFrame> filt_frame(av_frame_alloc(), [](AVFrame* p)
			{
				av_frame_free(&p);
			});

			const auto ret = av_buffersink_get_frame(video_graph_out_, filt_frame.get());
						
            if(ret == AVERROR_EOF)
			{
				if(enc->codec->capabilities & CODEC_CAP_DELAY)
				{
					while(encode_av_frame(video_st_->codec, avcodec_encode_video2, nullptr))
						boost::this_thread::yield(); // TODO:
				}		
			}
			else if(ret != AVERROR(EAGAIN))
			{
				FF_RET(ret, "av_buffersink_get_frame");
				if (filt_frame->interlaced_frame) 
				{
					if (enc->codec->id == AV_CODEC_ID_MJPEG)
						enc->field_order = filt_frame->top_field_first ? AV_FIELD_TT : AV_FIELD_BB;
					else
						enc->field_order = filt_frame->top_field_first ? AV_FIELD_TB : AV_FIELD_BT;
				} 
				else
					enc->field_order = AV_FIELD_PROGRESSIVE;

				filt_frame->quality = enc->global_quality;

				if (!enc->me_threshold)
					filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
			
				encode_av_frame(video_st_->codec, avcodec_encode_video2, filt_frame);
			}

			boost::this_thread::yield(); // TODO:
		}
	}
					
	void encode_audio(const std::shared_ptr<core::read_frame>& frame_ptr)
	{		
		if(!audio_st_)
			return;
		
		auto enc = audio_st_->codec;
									
		if(frame_ptr)
		{
			std::shared_ptr<AVFrame>  src_av_frame(av_frame_alloc(), [](AVFrame* p)
			{
				av_frame_free(&p);
			});
		
			src_av_frame->channels		 = 2; // TODO:
			src_av_frame->channel_layout = av_get_default_channel_layout(2); // TODO:
			src_av_frame->sample_rate	 = in_video_format_.audio_sample_rate;
			src_av_frame->nb_samples	 = frame_ptr->audio_data().size() / src_av_frame->channels;
			src_av_frame->format		 = AV_SAMPLE_FMT_S32;

			FF(av_samples_fill_arrays(src_av_frame->extended_data, 
									  src_av_frame->linesize,
									  reinterpret_cast<const std::uint8_t*>(&*frame_ptr->audio_data().begin()), 
									  2, // TODO
									  src_av_frame->nb_samples, 
									  AV_SAMPLE_FMT_S32, 
									  16)); 
					
			FF(av_buffersrc_add_frame(audio_graph_in_, src_av_frame.get()));
		}

		while(true)
		{
			std::shared_ptr<AVFrame> filt_frame(av_frame_alloc(), [](AVFrame* p)
			{
				av_frame_free(&p);
			});

			const auto ret = av_buffersink_get_frame(audio_graph_out_, filt_frame.get());

			filt_frame->pts = AV_NOPTS_VALUE; // TODO:
			
            if(ret == AVERROR(EAGAIN))
                break;

			if(ret == AVERROR_EOF)
			{
				if(enc->codec->capabilities & CODEC_CAP_DELAY)
				{
					while(encode_av_frame(audio_st_->codec, avcodec_encode_audio2, nullptr))
						boost::this_thread::yield(); // TODO:
				}
			}
			else if(ret != AVERROR(EAGAIN))
			{
				FF_RET(ret, "av_buffersink_get_frame");

				encode_av_frame(audio_st_->codec, avcodec_encode_audio2, filt_frame);
			}

			boost::this_thread::yield(); // TODO:
		}
	}
	
	template<typename F>
	bool encode_av_frame(AVCodecContext* enc, const F& func, const std::shared_ptr<AVFrame>& src_av_frame)
	{
		std::shared_ptr<AVPacket> pkt_ptr(new AVPacket(), [](AVPacket* p){av_free_packet(p); delete p;});
		av_init_packet(pkt_ptr.get());

		int got_packet = 0;
		FF(func(enc, pkt_ptr.get(), src_av_frame.get(), &got_packet));
					
		if(!got_packet || pkt_ptr->size <= 0)
			return false;
				
		pkt_ptr->stream_index = audio_st_->index;
		
		write_packet(std::move(pkt_ptr));

		return true;
	}

	void write_packet(const std::shared_ptr<AVPacket>& pkt_ptr)
	{		
		if(pkt_ptr)
		{
			auto st  = oc_->streams[pkt_ptr->stream_index];
			auto enc = st->codec;

			if (pkt_ptr->pts != AV_NOPTS_VALUE)
				pkt_ptr->pts = av_rescale_q(pkt_ptr->pts, enc->time_base, st->time_base);

			if (pkt_ptr->dts != AV_NOPTS_VALUE)
				pkt_ptr->dts = av_rescale_q(pkt_ptr->dts, enc->time_base, st->time_base);
				
			if (st->codec->codec_type == AVMEDIA_TYPE_AUDIO && pkt_ptr->dts != AV_NOPTS_VALUE) 
			{
				int duration = av_get_audio_frame_duration(enc, pkt_ptr->size);
				if(!duration)
					duration = enc->frame_size;

				AVRational fs_tb = {1, enc->sample_rate};
				pkt_ptr->dts = av_rescale_delta(enc->time_base, pkt_ptr->dts, fs_tb, duration, &filter_in_rescale_delta_last_, st->time_base);
			}

			pkt_ptr->duration = static_cast<int>(av_rescale_q(pkt_ptr->duration, enc->time_base, st->time_base));
		}

		FF(av_interleaved_write_frame(oc_.get(), pkt_ptr.get()));	
	}	

	void parse_options(AVDictionary** opts, std::string expr)
	{
		std::for_each(
			boost::sregex_iterator(
				configuration_.options.begin(),
				configuration_.options.end(),
				boost::regex(expr, boost::regex::icase)),
			boost::sregex_iterator(),
			[&](const boost::match_results<std::string::const_iterator>& what)
			{
				av_dict_set(opts, what["NAME"].str().c_str(), what["VALUE"].str().c_str(), 0);
			});
	}

	template<class T>
	boost::optional<T> get_option(std::string name)
	{
		boost::smatch what;
		if(boost::regex_search(configuration_.options, what, boost::regex("-" + name + "\\s(?<VALUE>[^\\s]*)")))
			return boost::lexical_cast<T>(what["VALUE"].str());

		return nullptr;
	}
	
	template<typename T>
	T get_option(std::string name, T default)
	{
		boost::smatch what;
		if(boost::regex_search(configuration_.options, what, boost::regex("-" + name + "\\s(?<VALUE>[^\\s]*)")))
			return boost::lexical_cast<T>(what["VALUE"].str());

		return default;
	}
};
	
safe_ptr<core::frame_consumer> create_consumer(const core::parameters& params)
{
    //auto str = std::accumulate(params.begin(), params.end(), std::wstring(), [](const std::wstring& lhs, const std::wstring& rhs) {return lhs + L" " + rhs;});
    //   
    //static boost::wregex path_exp(L"\\s*(FILE\\s)?(?<PATH>.+\\.[^\\s]+|.+:[^\\s]*)?\\s*(?<OPTIONS>-.*)?" , boost::regex::icase);

    //boost::wsmatch what;
    //if(!boost::regex_match(str, what, path_exp))
    //     return core::frame_consumer::empty();
	
	ffmpeg_consumer::configuration configuration;

	//configuration.filename = narrow(what["PATH"].str());
                           
    return make_safe<ffmpeg_consumer>(configuration);
}

safe_ptr<core::frame_consumer> create_consumer(const boost::property_tree::wptree& ptree)
{              
	ffmpeg_consumer::configuration configuration;

	configuration.filename = narrow(ptree.get<std::wstring>(L"path"));
	configuration.options  = narrow(ptree.get<std::wstring>(L"options", L""));
	
    return make_safe<ffmpeg_consumer>(configuration);
}

}}