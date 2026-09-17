using System; using System.IO; using System.Text; using System.Collections.Generic; using System.Linq; using System.Net; using System.Web;

namespace w{public static class l{

public static string 

	getURL(string URL, string userAgent="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/75.0.3770.100 Safari/537.36\n"){
		var wc = new WebClient();
		wc.Headers.Add("user-agent",userAgent);
		Stream s = wc.OpenRead(URL);
		return new StreamReader(s).ReadToEnd();}
		
		
public static int xc = 3732;

public static double total = 0.0;
public static double done = 0.0;
public static double percent = 0.0;
public static double mega = (1024.0*1024.0);

public static string prefix="https://is.gd/";
public static string xd = "http://ok.com/?";
public static string fzd = "forward.php?format=simple&shorturl=";
public static string czd = "create.php?format=simple&url=";
public static string 

	uploadChunk(byte[] data){
		if (data.Length > (xc+1))
			throw new Exception("Chunk too large : " + data.Length);
		var wid = "";try{
			string b64 = Convert.ToBase64String(data);
			string rl = prefix + czd + (xd +b64.Replace("+",".").Replace("/","_"));
			string response = getURL(rl);
			wid = response.Substring(prefix.Length,6);	
		}catch(Exception ex){
			throw new Exception(ex + "Server connection Failed");}
		return wid;}
public static int curpercent = 0;

public static string 

	uploadBytes(string filename, byte[] data){
		
		var hashes = "";
		
		var start = 0; 
		var stop = data.Length;
		while(start<stop){
			var chunk = new ArraySegment<byte>(data, start, (xc>(stop-start)?(stop-start):xc)).ToArray();
			start +=xc;
			if (!filename.StartsWith("Fle:")){
				done += (double)xc;
				percent = done/total*100.0;;
				if ((Convert.ToInt32(percent)>>0)>curpercent){curpercent=Convert.ToInt32(percent)>100?100:Convert.ToInt32(percent);
				Console.WriteLine(">> " + curpercent + "%");
			}}
				
			if (hashes!="") hashes+=",";
			hashes += uploadChunk(chunk);}
			
		if (hashes.Length<xc){
			return uploadChunk(Encoding.UTF8.GetBytes("Fle:" + filename +"," +hashes));	
		}else{
			hashes = "Fle:" + filename +"," + hashes;
			return getBytesHash(Encoding.UTF8.GetBytes(hashes));}}
			

public static String 

	getBytesHash(byte[] data){ 
	
		var hashes = "";
		var start = 0; 
		var stop = data.Length;
		while(start<stop){
			var chunk = new ArraySegment<byte>(data, start, (xc>(stop-start)?(stop-start):xc)).ToArray();	
			start +=xc;
			if (hashes!="") hashes+=",";hashes += uploadChunk(chunk);}
			
		if (hashes.Length <xc){
			return uploadChunk(Encoding.UTF8.GetBytes("Mlt:"+data.Length+"," +hashes));	
		}else{
			return getBytesHash(Encoding.UTF8.GetBytes(hashes));}}
			

public static String 

	getFileHash(string fileFolder, string wcards = "*.*"){
		
		var isFolder = Directory.Exists(fileFolder);
		if (isFolder){
			
			String hashes = "";
			
			String[] folder = Directory.GetFiles(fileFolder, wcards, SearchOption.AllDirectories);
			foreach(string file in folder) total += (double) new FileInfo(file).Length;
			Console.WriteLine("Total to upload : " + (total / mega) + " Mbytes.");
			
			foreach(string file in folder){	
			    Console.WriteLine("Uploading: " +file + "...");
				if (hashes!="") hashes+=",";
				hashes += "Fld:" + Path.GetDirectoryName(file) + ":" + getFileHash(file);}
				
			return getBytesHash(Encoding.UTF8.GetBytes(hashes));}
			
		else{
			
			var filename = Path.GetFileName(fileFolder);
			if (filename.Contains("*"))
				return getFileHash(Path.GetDirectoryName(fileFolder), filename);
			var data=File.ReadAllBytes(fileFolder);
			if (total<1.0) total = (double)data.Length;
			
			return uploadBytes(Path.GetFileName(fileFolder), data);}}
				

private static void 

	decodeFile(string fileName, IEnumerable<string> ids, string strFolder = ""){
		
		if (fileName.StartsWith("Mlt"))
			dlRecurse(Encoding.UTF8.GetString(decodeMulti(ids, Convert.ToInt64(fileName.Split(':')[1]))));
		
		if(!String.IsNullOrEmpty(strFolder))
			if (!Directory.Exists(strFolder))
				Directory.CreateDirectory(strFolder);
			
		var writer = new BinaryWriter(File.OpenWrite(Path.Combine("/storage/emulated/0/Download",fileName)));
		Console.WriteLine("decoding : "+fileName+" ...");
		foreach(string wid in ids){
			if (!String.IsNullOrEmpty(wid)){
			try{
				string strok = getURL(prefix + fzd + wid);
				byte[] data = Convert.FromBase64String(strok.Substring(xd.Length).Replace(".","+").Replace("_","/"));
				writer.Write(data);
		}catch(Exception ex){ex.ToString();}}}
		writer.Flush();
		writer.Close();}
		

private static byte[] 

	decodeMulti(IEnumerable<string> ids, long length){
		
		var ms = new MemoryStream((int)length);
		var writer = new BinaryWriter(ms);
		foreach(string wid in ids){
			string strok = getURL(prefix + fzd  + wid);
			byte[] data = Convert.FromBase64String(strok.Substring(xd.Length).Replace(".","+").Replace("_","/"));
			writer.Write(data);}
		ms.Seek(0, SeekOrigin.Begin);
		var result = new byte[length];
		ms.Read(result, 0, (int)length);
		return result;}
		
		
private static void 

	dlRecurse(string data, string strFolder = ""){
		
		var sd = data.Split(',');
		var fd = sd[0].Split(':');
		if (data.StartsWith("Fle:")){
			decodeFile(String.Join(":", fd.Skip(1)), sd.Skip(1).ToArray(), strFolder);}
		else if (data.StartsWith("Fld:")){
			foreach(string fle in sd)
			{	var fde = fle.Split(':');
				downloadFileOrFolder(fde[2],fde[1]);}}
		else if (data.StartsWith("Mlt:")){
			var mdata = decodeMulti(sd.Skip(1).ToArray(), Convert.ToInt64(fd[1]));
			dlRecurse(Encoding.UTF8.GetString(mdata));}}


public static void 

	downloadFileOrFolder(string wid, string strFolder = ""){
		
		string strok = getURL(prefix + fzd + wid);
		byte[] bdata = Convert.FromBase64String(strok.Substring(xd.Length).Replace(".","+").Replace("_","/"));
		string data = Encoding.UTF8.GetString(bdata);
		
		dlRecurse(data, strFolder);}
		
}
public class m{public static void Main(String[] a){try{	
	
	a=new String[]{"-get","housemusic"};
	var o=new Dictionary<String,Func<String, String>>{

		{ "",(filename) => {
			if(filename.StartsWith("-"))throw new Exception("Unknown Option : " + filename);else{Console.WriteLine("Uploading..."); 
			
			var result = l.getFileHash(filename);
			
			return "Wise folder {" + filename + "} uploaded to wid : " + result; }}},


		{ "-get",(wid) => {
			Console.WriteLine("Downloading..."); 
			
			l.downloadFileOrFolder(wid); 
			
			return "Wise id {"+wid+"} download ok." ; }}};
		
		
		if (a.Length<1){
			throw new Exception("Nothing to do.");}


		//Porshe 911 Carrera
		if (o.ContainsKey(a[0]))Console.WriteLine(o[a[0]](a.Length>1 ? a[1] : ""));
		else Console.WriteLine(o[""](a[0]));}


		catch(Exception ex){
			Console.WriteLine(ex + usage());}}


public static string 
	usage(){return "\n\n usage : xfold C:\\myfolder\n         xfold -get MYF7C8";}}}