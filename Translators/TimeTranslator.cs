// Author: Pantelis Andrianakis
// Creation Date: October 3rd 2024

using System.Text;
using System.Text.RegularExpressions;

namespace Breezy.Translators
{
	class TimeTranslator : MethodLibrary
	{
		public static string Process(string source)
		{
			// Support for random method names to avoid conflicts.
			string random = GetRandomMethodIdentifier();

			// Define the regex patterns to find Time.current, Time.timeString, Time.dateString and Time.dateTimeString.
			string currentPattern = @"Time\.(?i)current\(\)";
			string timeStringPattern = @"Time\.(?i)timeString\(\)";
			string dateStringPattern = @"Time\.(?i)dateString\(\)";
			string dateTimeStringPattern = @"Time\.(?i)dateTimeString\(\)";

			bool foundCurrent = false;
			bool foundTimeString = false;
			bool foundDateString = false;
			bool foundDateTimeString = false;

			// Replace Time.current with timeCurrent and track if found.
			source = Regex.Replace(source, currentPattern, match =>
			{
				foundCurrent = true;
				return $"timeCurrent{random}()";
			});

			// Replace Time.timeString with timeTimeString and track if found.
			source = Regex.Replace(source, timeStringPattern, match =>
			{
				foundTimeString = true;
				return $"timeTimeString{random}()";
			});

			// Replace Time.dateString with timeDateString and track if found.
			source = Regex.Replace(source, dateStringPattern, match =>
			{
				foundDateString = true;
				return $"timeDateString{random}()";
			});

			// Replace Time.dateTimeString with timeDateTimeString and track if found.
			source = Regex.Replace(source, dateTimeStringPattern, match =>
			{
				foundDateTimeString = true;
				return $"timeDateTimeString{random}()";
			});

			// Add the necessary C++ methods if they are used.
			StringBuilder methods = new StringBuilder();

			// Check if we need to add the <regex> import.
			if (foundCurrent || foundTimeString || foundDateString || foundDateTimeString)
			{
				source = AddInclude(source, "chrono");
				if (foundTimeString || foundDateString || foundDateTimeString)
				{
					source = AddInclude(source, "ctime");
					source = AddInclude(source, "sstream");
					source = AddInclude(source, "iomanip");
				}
			}

			// Append timeCurrent method if it was found.
			if (foundCurrent)
			{
				methods.AppendLine($"long timeCurrent{random}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tauto duration = now.time_since_epoch();");
				methods.AppendLine("\treturn std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();");
				methods.AppendLine("}\n");
			}

			// Append timeTimeString method if it was found.
			if (foundTimeString)
			{
				methods.AppendLine($"std::string timeTimeString{random}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%H:%M:%S\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeDateString method if it was found.
			if (foundDateString)
			{
				methods.AppendLine($"std::string timeDateString{random}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%Y-%m-%d\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Append timeDateTimeString method if it was found.
			if (foundDateTimeString)
			{
				methods.AppendLine($"std::string timeDateTimeString{random}()");
				methods.AppendLine("{");
				methods.AppendLine("\tauto now = std::chrono::system_clock::now();");
				methods.AppendLine("\tstd::time_t currentTime = std::chrono::system_clock::to_time_t(now);");
				methods.AppendLine("\tstd::tm* timeinfo = std::localtime(&currentTime);");
				methods.AppendLine("\tstd::stringstream ss;");
				methods.AppendLine("\tss << std::put_time(timeinfo, \"%Y-%m-%d %H:%M:%S\");");
				methods.AppendLine("\treturn ss.str();");
				methods.AppendLine("}\n");
			}

			// Parent class manages adding the additional methods.
			source = AddMethods(source, methods.ToString());

			return source;
		}
	}
}
